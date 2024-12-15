#include <assert.h>
#include <iso646.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_intsup.h>
#include <sys/_types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h> 
#include <esp_http_server.h>
#include "esp_eth_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp32-dht11.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/ip_addr.h"


#define CONFIG_SNTP_TIME_SERVER		"pool.ntp.org"
#define ESP_WIFI_SSID 				"ESP_DHT11"
#define ESP_WIFI_CHANNEL 			1
#define MAX_STA_CONN 				1

#define ESP_MAXIMUM_RETRY 			3

#define WIFI_CONNECTED_BIT 			BIT0
#define WIFI_FAIL_BIT      			BIT1

#define CONFIG_DHT11_PIN 			GPIO_NUM_9
#define CONFIG_CONNECTION_TIMEOUT 	5

#define HTTP_QUERY_KEY_MAX_LEN 		100  

#define MAX_MEASURMENT_TEMP			30
#define MAX_MEASURMENT_FILE			3000

#define value_chart_get(tab, num, pos, max) 	tab[((pos-num)>=0)?(pos-num):(max-num)]

static const char *TAG_AP = "wifi softAP";
static const char *TAG_ST = "wifi station";
static const char *TAG_WS = "esp32 webserver";
static const char *TAG_NV = "nvs api";
static const char *TAG_ME = "spiffs memory";
static const char *TAG_DH = "dht saved";
static const char *TAG_TI = "sntp_server";
static const char *TAG_TS = "thingspeak";

char *wifi_ssid = NULL;
char *wifi_pass = NULL;
char *APIkey = NULL;
uint8_t TIME_msr = 2;
bool TS_active = false;

fpos_t file_pos = 0;
uint32_t saved_count = 0;
uint8_t measurment_position = 0;
 
FILE* file = NULL;

char thingspeak_url[] = "https://api.thingspeak.com/update";
char data[] = "api_key=%s&field1=%.2f&field2=%.2f";

static EventGroupHandle_t s_wifi_event_group;

esp_http_client_handle_t client;

esp_vfs_spiffs_conf_t conf_memory = { .base_path = "/spiffs",
							   		  .partition_label = NULL,
      						   		  .max_files = 5,
      						          .format_if_mount_failed = true };

SemaphoreHandle_t DHT_11_Mutex;
SemaphoreHandle_t file_Mutex;
SemaphoreHandle_t send_Mutex;
SemaphoreHandle_t memory_Mutex;
nvs_handle_t my_nvs_handle;  

static int s_retry_num = 0;

float temp;
float hum;

float temp_memory[MAX_MEASURMENT_TEMP] = {0};
float hum_memory[MAX_MEASURMENT_TEMP] = {0};



/*--------------------------------------------------------- HTML section ----------------------------------------------------------------*/

char main_page[] =   "<!DOCTYPE HTML><html>\n"
                     "<head>\n"
                     "  <title>ESP-IDF DHT11 Web Server</title>\n"
                     "  <meta http-equiv='refresh' content='10 url=/'>\n"
                     "  <meta name='viewport' content='width=device-width, initial-scale=1'>\n"
                     "  <style>\n"
                     "    html {font-family: Arial; display: inline-block; text-align: center;}\n"
                     "    p {  font-size: 1.2rem;}\n"
                     "    body {  margin: 0;}\n"
                     "    .topnav { overflow: hidden; background-color: #241d4b; color: white; font-size: 1.7rem; }\n"
                     "    .content { padding: 20px; }\n"
                     "    .card { background-color: white; box-shadow: 2px 2px 12px 1px rgba(140,140,140,.5); }\n"
                     "    .cards { max-width: 700px; margin: 0 auto; display: grid; grid-gap: 2rem; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); }\n"
                     "    .reading { font-size: 2.8rem; }\n"
                     "    .card.temperature { color: #0e7c7b; }\n"
                     "    .card.humidity { color: #17bebb; }\n"
                     "    .button { width: 100%; padding: 15px 50px; font-size: 1.8rem; text-align: center; outline: none; color: #fff; background-color: #0ffa6d; border: #0ffa6d; border-radius: 5px;}\n"	
                     "    .button:active { background-color: #fa0f0f; transform: translateY(2px);}\n"
                     "  </style>\n"
                     "</head>\n"
                     "<body>\n"
                     "  <div class='topnav'>\n"
                     "    <h3>ESP-IDF DHT11 WEB SERVER</h3>%s\n"
                     "  </div>\n"
                     "  <div class='content'>\n"
                     "    <div class='cards'>\n"
                     "      <div class='card temperature'>\n"
                     "        <h4>TEMPERATURE</h4><p><span class='reading'>%.2f&deg;C</span>\n"
                     "      </div>\n"   
                     "      <div class='card humidity'>\n"
                     "        <h4>HUMIDITY</h4><p><span class='reading'>%.2f</span> &percnt;</span>\n"
                     "      </div>\n"
                     "      <a href='/value' download='value.csv' class='button'><button class='button'>POBIERZ</button></a>\n"
                     "      <a href='/chart' class='button'><button class='button'>WYKRES</button></a>\n"
                     "      <a href='/settings' class='button'><button class='button'>USTAWIENIA</button></a>\n"
                     "      <a href='/reset_menu' class='button'><button class='button'>RESET</button></a>\n"                   
                     "    </div>\n"
                     "  </div>\n"
                     "</body>\n"
                     "</html>";
                     
char settin_page[] = "<!DOCTYPE html>\n"
                     "<html>\n"
                     "<head>\n"
                     "	<title>ESP-IDF DHT11 Web Server</title>\n"
                     "	<meta name='viewport' content='width=device-width, initial-scale=1'>\n"
                     "	<style>\n"
                     "		html {font-family: Arial; display: inline-block; text-align: center; }\n"
                     "		p {  font-size: 1.2rem;}\n"
                     "		body {  margin: 0;}\n"
                     "		.topnav { overflow: hidden; background-color: #241d4b; color: white; font-size: 1.7rem; }\n"
                     "		.content { padding: 20px; }\n"
                     "		.card { background-color:  #abfff9; box-shadow: 2px 2px 12px 1px rgba(140,140,140,.5); }\n"
                     "		.cards { max-width: 700px; margin: 0 auto; display: grid; grid-gap: 2rem; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); }\n"
                     "		.reading {box-sizing : border-box; font-size: 2.0rem; text-align: center; }\n"
                     "		.button {box-sizing : border-box; width: 90%; max-width: 700px; padding: 15px 50px; font-size: 1.8rem; text-align: center; outline: none; color: #fff; background-color: #0ffa6d; border: #0ffa6d; border-radius: 5px; }\n"
                     "		.button:active { background-color: #fa0f0f; transform: translateY(2px);}\n"
                     "		.largerCheckbox { width: 80px; height: 40px;}\n"
                     "	</style>\n"
                     "</head>\n"
                     "<body>\n"
                     "	<div class='topnav'>\n"
                     "    <h3>ESP-IDF DHT11 WEB SERVER</h3>%s\n"
                     "	</div>\n"
                     "	<div class='content'>\n"
                     "	<div class='cards'>\n"
                     "	<div class='card'>\n"
                     "		<form action='/saved' method='post'><br>\n"
                     "			<label for='SSID' class='reading'>Nazwa sieci:</label><br>\n"
                     "			<input type='text' id='SSID' name='SSID' value='%s' class='reading' size='10'><br><br>\n"
                     "			<label for='PASS' class='reading'>Haslo do sieci:</label><br>\n"
                     "			<input type='text' id='PASS' name='PASS' value='%s' class='reading' size='10'><br><br>\n"
	                 "			<label for='TIME_msr' class='reading'>Czas pomiaru: </label>\n"
                     "			<input type='number' id='TIME_msr' name='TIME_msr' min='2' max='60' class='reading' value='%d'><br><br>\n"
                     "			<label for='TS_active'' class='reading'>Uzywaj ThingSpeak:</label>\n"
                     "			<input type='checkbox' id='TS_active' name='TS_active' value='yes' class='largerCheckbox' %s><br><br>\n"
 					 "			<label for='APIkey' class='reading'>API key:</label><br>\n"
                     "			<input type='text' id='APIkey' name='APIkey' value='%s' class='reading' size='10'><br><br>\n"
                     "			<input type='submit' value='ZAPISZ' class='button'><br><br>\n"
                     "		</form>\n" 
                     "	</div>\n"
                     "	</div>\n"
                     "	</div>\n"
                     "</body>\n"
                     "</html>";

char  saved_page[] = "<!DOCTYPE html>\n"
                     "<html>\n"
                     "<head>\n"
                     "	<title>ESP-IDF DHT11 Web Server</title>\n"
                     "  <meta http-equiv='refresh' content='10 url=/reset'>\n" 
                     "  <meta name='viewport' content='width=device-width, initial-scale=1'>\n"      
                     "	<style>\n"
                     "		html {font-family: Arial; display: inline-block; text-align: center;}\n"
                     "		body {  margin: 0;}\n"
                     "		.topnav { overflow: hidden; background-color: #241d4b; color: white; font-size: 1.7rem; }\n"
                     "		.content { padding: 20px; }\n"
                     "		.card { background-color:  #e6f2ff; box-shadow: 2px 2px 12px 1px rgba(140,140,140,.5); }\n"
                     "		.one { color: #0ffa6d; }\n"
                     "		.cards { margin: 0 auto; display: grid; grid-gap: 2rem; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); }\n"
                     "	</style>\n"
                     "</head>\n"
                     "<body>\n"
                     "	<div class='topnav'>\n"
                     "    <h3>ESP-IDF DHT11 WEB SERVER</h3>%s\n"
                     "  </div>\n"
                     "	<div class='content'>\n"
                     "		<div class='cards'>\n"
                     "			<div class='card'>\n"
                     "				<div class='one'><h1>ZAKONCZONO SUCCESEM</h1><br></div>\n"
                     "				<h3>za 30s nastapi reset urzadzenia</h3>\n"
                     "			</div>\n"
                     "		</div>\n"
                     "	</div>\n"
                     "</body>\n"
                     "</html>";              

char res_me_page[] = "<!DOCTYPE html>\n"
                     "<html>\n"
                     "<head>\n"
                     "	<title>ESP-IDF DHT11 Web Server</title>\n"
                     "  <meta name='viewport' content='width=device-width, initial-scale=1'>\n"
                     "	<style>\n"
                     "		html {font-family: Arial; display: inline-block; text-align: center;}\n"
                     "		p {  font-size: 1.2rem;}\n"
                     "		body {  margin: 0;}\n"
                     "		.topnav { overflow: hidden; background-color: #241d4b; color: white; font-size: 1.7rem; }\n"
                     "		.content { padding: 20px; }\n"
                     "		.card { background-color:  #abfff9; box-shadow: 2px 2px 12px 1px rgba(140,140,140,.5); }\n"
                     "		.cards { max-width: 700px; margin: 0 auto; display: grid; grid-gap: 2rem; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); }\n"
                     "		.reading { width: 80%; font-size: 2.0rem; text-align: center; }\n"
                     "		.button { box-sizing : border-box; width: 80%;  padding: 15px 50px; font-size: 1.8rem; text-align: center; outline: none; color: #fff; background-color: #0ffa6d; border: #0ffa6d; border-radius: 5px;}\n"
                     "		.button:active { background-color: #fa0f0f; transform: translateY(2px);}\n"
                     "	</style>\n"
                     "</head>\n"
                     "<body>\n"
                     "	<div class='topnav'>\n"
                     "    <h3>ESP-IDF DHT11 WEB SERVER</h3>%s\n"
                     "    </div>\n"
                     "	<div class='content'>\n"
                     "		<div class='cards'>\n"
                     "		<div class='card'>\n"
                     "			<br><a href='/reset_only'><button class='button'>TYLKO RESET</button></a><br>\n"
                     "			<br><a href='/format_flash'><button class='button'>USUN POMIARY</button></a><br>\n"
                     "			<br><a href='/default_settings'><button class='button'>RESET FABRYCZNY</button></a><br><br>\n"
                     "		</div>\n"
                     "		</div>\n"
                     "	</div>\n"
                     "</body>\n"
                     "</html>\n";

char   chrt_page[] = "<!DOCTYPE html>\n"
                     "<html>\n"
                     "<script src='https://cdnjs.cloudflare.com/ajax/libs/Chart.js/2.9.4/Chart.js'></script>\n"
                     "<head>\n"
                     "	<title>ESP-IDF DHT11 Web Server</title>\n"
                     "  <meta name='viewport' content='width=device-width, initial-scale=1'>      \n"
                     "	<style>\n"
                     "		html {font-family: Arial; display: inline-block; text-align: center;}\n"
                     "		body {  margin: 0;}\n"
                     "		.topnav { overflow: hidden; background-color: #241d4b; color: white; font-size: 1.7rem; }\n"
                     "		.content { padding: 20px; }\n"
                     "		.card { background-color:  #e6f2ff; box-shadow: 2px 2px 12px 1px rgba(140,140,140,.5); }\n"
                     "		.cards { margin: 0 auto; display: grid; grid-gap: 2rem; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); max-width:1000px; }\n"
                     "	</style>\n"
                     "</head>\n"
                     "<body>\n"
                     "	<div class='topnav'>\n"
                     "    <h3>ESP-IDF DHT11 WEB SERVER</h3>%s\n"
                     "  </div>\n"
                     "	<div class='content'>\n"
                     "		<div class='cards'>\n"
                     "			<div class='card'>\n"
                     "				<center><canvas id='myChart' style='width:100%;max-width:1000px'></canvas></center>\n"
                     "				<p><span style='color:red; font-weight:bold'> Temperatura &#x2015; </span>&emsp;&emsp;\n"
				   	 "			       <span style='color:blue; font-weight:bold'> Wilgotnosc &#x2015; </span></p>\n"
                     "			</div>\n"
                     "		</div>\n"
                     "	</div>\n"
                     "<script>\n"
					 "const msn = %d\n"
					 "const xValues = [-1*msn,-2*msn,-3*msn,-4*msn,-5*msn,-6*msn,-7*msn,-8*msn,-9*msn,-10*msn,-11*msn,-12*msn,-13*msn,-14*msn,-15*msn,-16*msn,-17*msn,-18*msn,-19*msn,-20*msn]\n"
                     "new Chart('myChart', {\n"
                     "  type: 'line',\n"
                     "  data: {\n"
                     "    labels: xValues,\n"
                     "    datasets: [{ \n"
                     "      data: %s,\n"
                     "      borderColor: 'red',\n"
                     "      fill: false\n"
                     "    }, { \n"
                     "      data: %s,\n"
                     "      borderColor: 'blue',\n"
                     "      fill: false\n"
                     "    }]\n"
                     "  },\n"
                     "  options: {\n"
                     "    legend: {display: false}\n"
                     "  }\n"
                     "});\n"
                     "</script>\n"
                     "</body>\n"
                     "</html>";

/*---------------------------------------------------------------------------------------------------------------------------------------*/



/*----------------------------------------------------------- thingspeak ----------------------------------------------------------------*/

void thingspeak_init()
{
	esp_http_client_config_t config = { .url = thingspeak_url,
										.method = HTTP_METHOD_GET,
										.auth_type = HTTP_AUTH_TYPE_BASIC,
										.transport_type = HTTP_TRANSPORT_OVER_SSL,
										.crt_bundle_attach = esp_crt_bundle_attach,
									  };
	client = esp_http_client_init(&config);
	esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");	
	ESP_LOGI(TAG_TS, "Message sent Failed hard");					  
}


void thingspeak_send_data(float temp, float hum)
{
	char api_key[32] = {0};
	char post_data[200];
	esp_err_t err;
	if(APIkey==NULL) return;
	strcpy(post_data, "");
	strcpy(api_key, APIkey);
	snprintf(post_data, sizeof(post_data), data, api_key, temp, hum);
	esp_http_client_set_post_field(client, post_data, strlen(post_data));

	err = esp_http_client_perform(client);

	if(err == ESP_OK)
	{
		int status_code = esp_http_client_get_status_code(client);
		if(status_code != 200) ESP_LOGI(TAG_TS, "Message sent Failed");
	}
	else
	{
		ESP_LOGI(TAG_TS, "Message sent Failed hard");
	}
}

/*---------------------------------------------------------------------------------------------------------------------------------------*/



/*----------------------------------------------------- SPIFFS handling -----------------------------------------------------------------*/

void memory_init(void)
{      							   
   	esp_err_t ret = esp_vfs_spiffs_register(&conf_memory);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG_ME, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG_ME, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG_ME, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }	
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf_memory.partition_label, &total, &used);
    if (ret != ESP_OK) 
    {
        ESP_LOGE(TAG_ME, "Failed to get SPIFFS partition information (%s). Formatting...", esp_err_to_name(ret));
        esp_spiffs_format(conf_memory.partition_label);
        return;
    } 
    else 
    {
        ESP_LOGI(TAG_ME, "Partition size: total: %d, used: %d", total, used);
    }
        
   	file = fopen("/spiffs/mesurment.txt", "r+");
    if (file == NULL) 
    {
        ESP_LOGI(TAG_ME, "Failed to open file for writing");
        file = fopen("/spiffs/mesurment.txt", "w+");
        if (file == NULL)
        {
         	ESP_LOGE(TAG_ME, "Failed create new file");
        	return;			
		}
    }
    else
    {
		fsetpos(file, &file_pos);	
		ESP_LOGI(TAG_ME, "position = %d", (int)file_pos);
	}
    ESP_LOGI(TAG_ME, "file OK");
}   

void memory_write(float* temp, float* hum)
{    
	time_t now;
    time(&now);
	xSemaphoreTake(send_Mutex, portMAX_DELAY);
    for (uint_fast8_t i=0; i<MAX_MEASURMENT_TEMP; i++) 
    {
		if((temp[i]>99.0)||(hum[i]>99.0)) continue;
		fprintf(file, "%.2f, %.2f, +%lld[s]\n", temp[i], hum[i], now-(i*TIME_msr));
	}
	fgetpos(file, &file_pos);
	fflush(file);
	saved_count++;
	if(saved_count >= MAX_MEASURMENT_FILE)
	{
		saved_count = 0; 
		file_pos = 0;
		fsetpos(file, &file_pos);
	} 
	xSemaphoreGive(send_Mutex);
	ESP_ERROR_CHECK(nvs_set_blob(my_nvs_handle, "saved_count", (void*)&saved_count, sizeof(uint32_t)));
	ESP_ERROR_CHECK(nvs_set_blob(my_nvs_handle, "file_pos", (void*)&file_pos, sizeof(fpos_t)));
}

void memory_clean(bool format)
{
	saved_count = 0;
    file_pos = 0;
    fclose(file);
   	ESP_ERROR_CHECK(nvs_set_blob(my_nvs_handle, "saved_count", (void*)&saved_count, sizeof(uint32_t)));
	ESP_ERROR_CHECK(nvs_set_blob(my_nvs_handle, "file_pos", (void*)&file_pos, sizeof(fpos_t)));
	ESP_LOGI(TAG_ME, "SPIFFS partition formatting...");
    if(format) esp_spiffs_format(conf_memory.partition_label);
    else  unlink("/spiffs/mesurment.txt");
    ESP_LOGI(TAG_ME, "SPIFFS done...");
}

/*---------------------------------------------------------------------------------------------------------------------------------------*/



/*------------------------------------------------------- NVS handling ------------------------------------------------------------------*/

void nvs_init(void)
{
	esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);	
}

esp_err_t nvs_read_netwoek_settings(nvs_handle_t handle, const char* key, char** out_buf, size_t max_size)
{
	char nvs_buf[max_size];
    switch (nvs_get_str(handle, key, nvs_buf,&max_size)) 
    {
    	case ESP_OK:
            ESP_LOGI(TAG_NV, "%s = %s", key, nvs_buf);
            *out_buf = malloc(max_size);
            memcpy(*out_buf, nvs_buf, max_size);
            return ESP_OK;
        	break;
    	case ESP_ERR_NVS_NOT_FOUND:
            ESP_LOGI(TAG_NV, "The %s value is not initialized yet!", key);
            return ESP_OK;
            break;
        default :
         	ESP_LOGI(TAG_NV, "Problem with memory");
         	return ESP_ERR_INVALID_RESPONSE;
    }	
}

esp_err_t nvs_read_data(nvs_handle_t handle, const char* key, void* out_buf, size_t size)
{
    switch (nvs_get_blob(handle, key, out_buf,&size)) 
    {
    	case ESP_OK:
            ESP_LOGI(TAG_NV, "%s is ok", key);
            return ESP_OK;
        	break;
    	case ESP_ERR_NVS_NOT_FOUND:
            ESP_LOGI(TAG_NV, "The %s value is not initialized yet!", key);
            return ESP_OK;
            break;
        default :
         	ESP_LOGI(TAG_NV, "Problem with memory");
         	return ESP_ERR_INVALID_RESPONSE;
    }	
}


void nvs_setup(void)
{  
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &my_nvs_handle));
    ESP_ERROR_CHECK(nvs_read_netwoek_settings(my_nvs_handle, "wifi_ssid", &wifi_ssid,HTTP_QUERY_KEY_MAX_LEN));
    ESP_ERROR_CHECK(nvs_read_netwoek_settings(my_nvs_handle, "wifi_pass", &wifi_pass,HTTP_QUERY_KEY_MAX_LEN));
    ESP_ERROR_CHECK(nvs_read_netwoek_settings(my_nvs_handle, "APIkey", &APIkey,HTTP_QUERY_KEY_MAX_LEN));
    ESP_ERROR_CHECK(nvs_read_data(my_nvs_handle, "file_pos", &file_pos, sizeof(fpos_t)));
    ESP_ERROR_CHECK(nvs_read_data(my_nvs_handle, "saved_count", &saved_count, sizeof(fpos_t)));
    ESP_ERROR_CHECK(nvs_read_data(my_nvs_handle, "TIME_msr", &TIME_msr, sizeof(TIME_msr)));
    ESP_ERROR_CHECK(nvs_read_data(my_nvs_handle, "TS_active", &TS_active, sizeof(TS_active)));
    ESP_LOGI(TAG_NV,"file_pos = %d", (int)file_pos);
    ESP_LOGI(TAG_NV,"saved_count = %d", (int)saved_count);
    ESP_LOGI(TAG_NV,"TIME_msr = %d", (int)TIME_msr);
    ESP_LOGI(TAG_NV,"TS_active = %d", (int)TS_active);
	if(APIkey) ESP_LOGI(TAG_NV,"APIkey = %s", APIkey);
}

void nvs_format()
{
	ESP_LOGI(TAG_ME, "NVS partition formatting...");
	ESP_ERROR_CHECK(nvs_flash_erase());
}

/*---------------------------------------------------------------------------------------------------------------------------------------*/



/*-----------------------------------------------------  Measurement --------------------------------------------------------------------*/
bool DHT_check_value(float t, float h)
{
	static float t_prew = 0.0;
	static float h_prew = 0.0;
	static bool first = true;
	static uint8_t cnt = 0;
	if((t<99.0) && (h<99.0) && (t>-99.0) && (h>0.0) && (((fabsf(fabsf(t_prew)-fabsf(t))<10.0) && (fabsf(h_prew-h)<10.0)) || first || (cnt>=3)))
	{
		cnt = 0;
		t_prew = t;
		h_prew = h;
		first = false;
		return false;
	}
	if(cnt>=3) 
	{
		cnt = 0;
		return false;
	}
	cnt++;
	vTaskDelay(pdMS_TO_TICKS(100));
	return true;
}


void DHT_readings(void *pvParameters)
{
	dht11_t dht11_sensor;
	uint8_t ms_correct = 0;
    dht11_sensor.dht11_pin = CONFIG_DHT11_PIN;
    ESP_LOGI(TAG_DH, "Start!!" );
    xSemaphoreTake(file_Mutex, portMAX_DELAY);
	while(true)
	{
		if (xSemaphoreTake(DHT_11_Mutex, portMAX_DELAY))
		{
			do 
			{
				if(!dht11_read(&dht11_sensor, CONFIG_CONNECTION_TIMEOUT))
				{  
    				temp = dht11_sensor.temperature;
    				hum = dht11_sensor.humidity;
    				xSemaphoreGive(DHT_11_Mutex);
				}
				else 
				{
					temp = 99.0;
					hum = 99.0;
				}
				ms_correct++;
			} while(DHT_check_value(temp,hum));
			temp_memory[measurment_position] = temp;
 			hum_memory[measurment_position] = hum;
 			measurment_position++;
 			if(measurment_position>=MAX_MEASURMENT_TEMP)
 			{
				measurment_position = 0; 
				xSemaphoreGive(file_Mutex);
				vTaskDelay(pdMS_TO_TICKS(100));
				ms_correct++;
				xSemaphoreTake(file_Mutex,portMAX_DELAY);
			} 
		}
		if(TS_active) thingspeak_send_data(temp, hum);
		vTaskDelay(pdMS_TO_TICKS((TIME_msr*1000)-((ms_correct-1)*100)));
	}
}

void DHT_save(void *pvParameters)
{
	float temp_save[MAX_MEASURMENT_TEMP];
	float hum_save[MAX_MEASURMENT_TEMP];
	while (true) 
	{
		xSemaphoreTake(file_Mutex, portMAX_DELAY);
		xSemaphoreTake(memory_Mutex, portMAX_DELAY);
		memcpy(temp_save, temp_memory, sizeof(temp_save));
		memcpy(hum_save, hum_memory, sizeof(hum_save));
		xSemaphoreGive(file_Mutex);
		xSemaphoreGive(memory_Mutex);
		memory_write(temp_save,hum_save);
		vTaskDelay(pdMS_TO_TICKS((TIME_msr*1000)+1000));
	}
}

void DHT_init()
{
	for(uint8_t i = 1; i<MAX_MEASURMENT_TEMP;i++)
	{
		temp_memory[i] = 100.0;
		hum_memory[i] = 100.0;
	}

	DHT_11_Mutex = xSemaphoreCreateMutex();
	file_Mutex = xSemaphoreCreateMutex();
	send_Mutex = xSemaphoreCreateMutex();
	memory_Mutex = xSemaphoreCreateMutex();
	xTaskCreate(DHT_readings, "DHT_task1", configMINIMAL_STACK_SIZE*10, NULL, 5, NULL);
	xTaskCreate(DHT_save,     "DHT_task2", configMINIMAL_STACK_SIZE*10, NULL, 5, NULL);
}
 
/*---------------------------------------------------------------------------------------------------------------------------------------*/



/*-----------------------------------------------------  WiFi handling ------------------------------------------------------------------*/

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
	if (event_base == WIFI_EVENT)
	{
		if(event_id == WIFI_EVENT_STA_START) 
		{
        	esp_wifi_connect();
    	} 		
		else if (event_id == WIFI_EVENT_STA_DISCONNECTED) 
    	{
			if (s_retry_num < ESP_MAXIMUM_RETRY) 
    		{
        		esp_wifi_connect();
            	s_retry_num++;
            	ESP_LOGI(TAG_ST, "retry to connect to the AP");
        	} 
			else 
        	{
            	xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        	}
       		ESP_LOGI(TAG_ST,"connect to the AP fail");
    	} 
	}
	else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) 
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_ST, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
	else if (event_id == WIFI_EVENT_AP_STACONNECTED) 
    {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG_AP, "station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);
    } 
	else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) 
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG_AP, "station "MACSTR" leave, AID=%d, reason=%d", MAC2STR(event->mac), event->aid, event->reason);
    }
}

void wifi_init_softap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = { .ap = { .ssid =ESP_WIFI_SSID,
            							  .ssid_len = strlen(ESP_WIFI_SSID),
            							  .channel = ESP_WIFI_CHANNEL,
            							  .max_connection =MAX_STA_CONN,
            							  .authmode = WIFI_AUTH_OPEN,
            							  .pmf_cfg = { .required = true, },
        								},
    							};
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG_AP, "wifi_init_softap finished. SSID:%s", ESP_WIFI_SSID);
}

bool wifi_init_sta(void)
{
	bool result = false;
	char ssid[32] = {0};
	char pass[64] = {0};
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_t* netif = NULL;
    netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
	
    wifi_config_t wifi_config = { .sta = {.threshold.authmode = WIFI_AUTH_WPA2_PSK,},};

	strcpy(ssid, wifi_ssid);
	strcpy(pass, wifi_pass);
	memcpy(&wifi_config.sta.ssid, ssid, sizeof(ssid));
	memcpy(&wifi_config.sta.password, pass, sizeof(pass));
 
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );
    ESP_LOGI(TAG_ST, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
           								   WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
          								   pdFALSE,
						 	               pdFALSE,
           								   portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
		ESP_LOGI(TAG_ST, "connected to ap SSID:%s password:%s", wifi_ssid, wifi_pass);
		result = true;
    }
    else if (bits & WIFI_FAIL_BIT) ESP_LOGI(TAG_ST, "Failed to connect to SSID:%s, password:%s", wifi_ssid, wifi_pass);
    else ESP_LOGE(TAG_ST, "UNEXPECTED EVENT");			


    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT,   IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,    instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
    
    if(!result) esp_netif_destroy_default_wifi(netif);
    return result;
}

void wifi_init(void)
{
	ESP_LOGI(TAG_ST, "ESP_WIFI_MODE_STA");
	ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if(wifi_ssid != NULL) ESP_LOGI(TAG_ST, "wifi_ssid= %s", wifi_ssid);
    if(wifi_pass != NULL) ESP_LOGI(TAG_ST, "wifi_pass= %s", wifi_pass);
    if((wifi_ssid == NULL) || (wifi_pass == NULL)) goto AP_MODE;
    if(!wifi_init_sta())
    {
AP_MODE:	
		ESP_LOGI(TAG_AP, "ESP_WIFI_MODE_AP");
    	wifi_init_softap();
	}	
}

/*---------------------------------------------------------------------------------------------------------------------------------------*/



/*------------------------------------------------------- SNTP handling -----------------------------------------------------------------*/

void sntp_notification(struct timeval *tv)
{
    ESP_LOGI(TAG_TI, "Notification of a time synchronization event");
}

void sntp_period(void *pvParameters)
{
	int retry = 0;
   	const int retry_count = 15;
	while(true)
	{
  		retry = 0;
		while (esp_netif_sntp_sync_wait( pdMS_TO_TICKS(2000)) != ESP_OK && ++retry < retry_count) {
        	ESP_LOGI(TAG_TI, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
   		}
   		ESP_LOGI(TAG_TI, "Time sync");
		vTaskDelay(pdMS_TO_TICKS(1000*3600));
	}
}

void sntp_custom_init()
{
	esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_SNTP_TIME_SERVER);	
	esp_netif_sntp_init(&config);
	config.sync_cb = sntp_notification; 
	setenv("TZ", "CET", 1);
    tzset();
    xTaskCreate(sntp_period, "sntp_task1", configMINIMAL_STACK_SIZE*10, NULL, 5, NULL);
 	ESP_LOGI(TAG_TI, "Configurated");
}

void sntp_tim_to_html(char* strftime_buf, size_t size)
{
	time_t now;
    struct tm timeinfo;
	time(&now);
	localtime_r(&now, &timeinfo);
    strftime(strftime_buf, size, "%c", &timeinfo);
}

/*---------------------------------------------------------------------------------------------------------------------------------------*/



/*---------------------------------------------------  Web server handling --------------------------------------------------------------*/

void run_reset(void *pvParameters)
{
	vTaskDelay(pdMS_TO_TICKS(1000));
	esp_restart();
}

esp_err_t send_main_web_page(httpd_req_t *req)
{
    int response;
    char response_data[sizeof(main_page) + 50];
    char strftime_buf[64];
    sntp_tim_to_html(strftime_buf, sizeof(strftime_buf));
    memset(response_data, 0, sizeof(response_data));
    if (xSemaphoreTake(DHT_11_Mutex, pdMS_TO_TICKS(5000)))
	{
    	sprintf(response_data, main_page, strftime_buf, temp, hum);
    	xSemaphoreGive(DHT_11_Mutex);
    }
    else 
    {
		sprintf(response_data, main_page, strftime_buf, 255.0, 255.0);
	}
    response = httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);
    return response;
}

esp_err_t send_settings_web_page(httpd_req_t *req)
{
    int response;
    char response_data[sizeof(settin_page) + 500];
    char strftime_buf[64];
    sntp_tim_to_html(strftime_buf, sizeof(strftime_buf));
    memset(response_data, 0, sizeof(response_data));
    
    sprintf(response_data, settin_page, strftime_buf,
    									((wifi_ssid) ? wifi_ssid : ""),
    									((wifi_pass) ? wifi_pass : ""),
    									TIME_msr,
    									((TS_active) ? "checked" : ""),
    									((APIkey) ? APIkey : ""));
    									
    response = httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);
    return response;
}

esp_err_t send_download_web_page(httpd_req_t *req)
{ 
    int response = 0;
    char downlo_page[1025];
    fpos_t pos_prev;
    const fpos_t pos_run = 0;
	if(xSemaphoreTake(send_Mutex, pdMS_TO_TICKS(5000)))
    {
    	fgetpos(file, &pos_prev);
    	fsetpos(file, &pos_run);
    	do
    	{
			memset(downlo_page, 0, sizeof(downlo_page));
			fread(downlo_page, sizeof(char), sizeof(downlo_page)-1, file);
			response = httpd_resp_send_chunk(req, downlo_page, strlen(downlo_page));
			if (response != ESP_OK) break;
		} while(!feof(file));
		httpd_resp_sendstr_chunk(req, NULL);
		fsetpos(file, &pos_prev);	
		xSemaphoreGive(send_Mutex);	
	}
	else response = httpd_resp_send_408(req);
    return response;
}

esp_err_t send_saved_web_page(httpd_req_t *req)
{
	int ret;
    int response;
	size_t buf_len;
    char*  buf;
    char param[HTTP_QUERY_KEY_MAX_LEN];
    char response_data[sizeof(saved_page) + 50];
    char strftime_buf[64];
    sntp_tim_to_html(strftime_buf, sizeof(strftime_buf));
    memset(response_data, 0, sizeof(response_data));
  	buf_len = req->content_len+1;
 	buf = malloc(buf_len);
    memset(buf,0,buf_len);
    ret = httpd_req_recv(req, buf, buf_len);
    if (ret <= 0) 
    {
    	if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
		return ESP_FAIL;
    }
    memset(param,0,sizeof(param));
    if (httpd_query_key_value(buf, "SSID", param, sizeof(param)) == ESP_OK) 
    {
    	if(strcmp("",param))
		{
			ESP_LOGI(TAG_WS, "NEW SSID=%s", param);
			ESP_ERROR_CHECK(nvs_set_str(my_nvs_handle, "wifi_ssid", param));
		}
    }
    memset(param,0,sizeof(param));
    if (httpd_query_key_value(buf, "PASS", param, sizeof(param)) == ESP_OK) 
    {
		if(strcmp("",param))
		{
	        ESP_LOGI(TAG_WS, "NEW PASWORD=%s", param);			
	        ESP_ERROR_CHECK(nvs_set_str(my_nvs_handle, "wifi_pass", param));
		}
    }
    memset(param,0,sizeof(param));
    if (httpd_query_key_value(buf, "APIkey", param, sizeof(param)) == ESP_OK) 
    {
		if(strcmp("",param))
		{
	        ESP_LOGI(TAG_WS, "NEW APIkey=%s", param);			
	        ESP_ERROR_CHECK(nvs_set_str(my_nvs_handle, "APIkey", param));
		}
    }
    memset(param,0,sizeof(param));
    if (httpd_query_key_value(buf, "TIME_msr", param, sizeof(param)) == ESP_OK) 
    {
		if(strcmp("",param))
		{
	        ESP_LOGI(TAG_WS, "NEW TIME_msr=%s", param);			
	        TIME_msr = atoi(param);
	        ESP_ERROR_CHECK(nvs_set_blob(my_nvs_handle, "TIME_msr", (void*)&TIME_msr, sizeof(TIME_msr)));
		}
    }
    memset(param,0,sizeof(param));
    ret = httpd_query_key_value(buf, "TS_active", param, sizeof(param));
    if (ret == ESP_OK) 
    {  
		ESP_LOGI(TAG_WS, "NEW TS_active=%s", param);	
		if(strcmp("yes",param)==0)
		{
	      	
	       	TS_active = true;
	        ESP_ERROR_CHECK(nvs_set_blob(my_nvs_handle, "TS_active", (void*)&TS_active, sizeof(TS_active)));	
		}
    }
    else if(ret == ESP_ERR_NOT_FOUND)
    {
		ESP_LOGI(TAG_WS, "NEW TS_active=NO");			
		TS_active = false;
	    ESP_ERROR_CHECK(nvs_set_blob(my_nvs_handle, "TS_active", (void*)&TS_active, sizeof(TS_active)));		
	}
   
    free(buf);
    sprintf(response_data, saved_page, strftime_buf);
    response = httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);
    return response;
}


esp_err_t send_reset_web_page(httpd_req_t *req)
{
    int response;
    ESP_LOGI(TAG_WS, "restart");
	response = send_main_web_page(req);
	xTaskCreate(run_reset, "reset_task", configMINIMAL_STACK_SIZE, NULL, 5, NULL);
    return response;
}

esp_err_t send_reset_menu_web_page(httpd_req_t *req)
{
    int response;
	char response_data[sizeof(res_me_page) + 50];
    char strftime_buf[64];
    sntp_tim_to_html(strftime_buf, sizeof(strftime_buf));
    memset(response_data, 0, sizeof(response_data));
    sprintf(response_data, res_me_page, strftime_buf);
    response = httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);
    return response;
}

esp_err_t send_reset_only_web_page(httpd_req_t *req)
{
    int response;
    char response_data[sizeof(saved_page) + 50];
    char strftime_buf[64];
    sntp_tim_to_html(strftime_buf, sizeof(strftime_buf));
    memset(response_data, 0, sizeof(response_data));
    sprintf(response_data, saved_page, strftime_buf);
    response = httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);
    return response;
}

esp_err_t send_format_flash_web_page(httpd_req_t *req)
{
    int response;
    char response_data[sizeof(saved_page) + 50];
    char strftime_buf[64];
    sntp_tim_to_html(strftime_buf, sizeof(strftime_buf));
    memset(response_data, 0, sizeof(response_data));
    xSemaphoreTake(memory_Mutex, portMAX_DELAY);
    xSemaphoreTake(send_Mutex, portMAX_DELAY);
	memory_clean(false);
    sprintf(response_data, saved_page, strftime_buf);
    response = httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);
    return response;
}

esp_err_t send_default_settings_web_page(httpd_req_t *req)
{
    int response;
    char response_data[sizeof(saved_page) + 50];
    char strftime_buf[64];
    sntp_tim_to_html(strftime_buf, sizeof(strftime_buf));
    memset(response_data, 0, sizeof(response_data));
    xSemaphoreTake(memory_Mutex, portMAX_DELAY);
    xSemaphoreTake(send_Mutex, portMAX_DELAY);
    memory_clean(true);
    nvs_flash_erase();
    sprintf(response_data, saved_page, strftime_buf);
    response = httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);
    return response;
}

esp_err_t send_chart_web_page(httpd_req_t *req)
{
	float temp_save[MAX_MEASURMENT_TEMP];
	float hum_save[MAX_MEASURMENT_TEMP];
	int8_t position_save;
    int response;
    char response_data[sizeof(chrt_page) + 500];
    char strftime_buf[64];
    char buf_tem[150] = "[";
    char buf_hum[150] = "[";
    char value_buf[10];
    sntp_tim_to_html(strftime_buf, sizeof(strftime_buf));
    memset(response_data, 0, sizeof(response_data));
	xSemaphoreTake(memory_Mutex, portMAX_DELAY);
	position_save = measurment_position-1;
	memcpy(temp_save, temp_memory, sizeof(temp_save));
	memcpy(hum_save, hum_memory, sizeof(hum_save));
	xSemaphoreGive(memory_Mutex);
	
	memset(value_buf, 0, sizeof(value_buf));
	sprintf(value_buf, "%.2f", value_chart_get(temp_save,0,position_save,MAX_MEASURMENT_TEMP));
	strcat(buf_tem, value_buf);
	memset(value_buf, 0, sizeof(value_buf));
	sprintf(value_buf, "%.2f", value_chart_get(hum_save,0,position_save,MAX_MEASURMENT_TEMP));
	strcat(buf_hum, value_buf);
	for(uint8_t i=1; i<20; i++)
	{
	    if((value_chart_get(temp_save,i,position_save,MAX_MEASURMENT_TEMP)>99.0)||(value_chart_get(hum_save,i,position_save,MAX_MEASURMENT_TEMP)>99.0)) 
	 		continue;
		memset(value_buf, 0, sizeof(value_buf));
		sprintf(value_buf, ",%.2f", value_chart_get(temp_save,i,position_save,MAX_MEASURMENT_TEMP));
		strcat(buf_tem, value_buf);
		memset(value_buf, 0, sizeof(value_buf));
		sprintf(value_buf, ",%.2f", value_chart_get(hum_save,i,position_save,MAX_MEASURMENT_TEMP));
		strcat(buf_hum, value_buf);
	}
	strcat(buf_tem, "]");
	strcat(buf_hum, "]");
    sprintf(response_data, chrt_page, strftime_buf, TIME_msr, buf_tem, buf_hum);
    response = httpd_resp_send(req, response_data, HTTPD_RESP_USE_STRLEN);
    return response;
}

httpd_uri_t uri_main = { .uri = "/",
    					 .method = HTTP_GET,
    					 .handler = send_main_web_page,
    					 .user_ctx = NULL};

httpd_uri_t uri_set = { .uri = "/settings",
    				    .method = HTTP_GET,
    				    .handler = send_settings_web_page,
    				    .user_ctx = NULL};

httpd_uri_t uri_dow = { .uri = "/value",
    				    .method = HTTP_GET,
    				    .handler = send_download_web_page,
    				    .user_ctx = NULL};

httpd_uri_t uri_sav = { .uri = "/saved",
    				    .method = HTTP_POST,
    				    .handler = send_saved_web_page,
    				    .user_ctx = NULL};

httpd_uri_t uri_res = { .uri = "/reset",
    				    .method = HTTP_GET,
    				    .handler = send_reset_web_page,
    				    .user_ctx = NULL};


httpd_uri_t uri_men = { .uri = "/reset_menu",
    				    .method = HTTP_GET,
    				    .handler = send_reset_menu_web_page,
    				    .user_ctx = NULL};
    				    
httpd_uri_t uri_onl = { .uri = "/reset_only",
    				    .method = HTTP_GET,
    				    .handler = send_reset_only_web_page,
    				    .user_ctx = NULL};    	
   
httpd_uri_t uri_frm = { .uri = "/format_flash",
    				    .method = HTTP_GET,
    				    .handler = send_format_flash_web_page,
    				    .user_ctx = NULL};  	
    				    		
httpd_uri_t uri_cln = { .uri = "/default_settings",
    				    .method = HTTP_GET,
    				    .handler = send_default_settings_web_page,
    				    .user_ctx = NULL};  

httpd_uri_t uri_chr = { .uri = "/chart",
    				    .method = HTTP_GET,
    				    .handler = send_chart_web_page,
    				    .user_ctx = NULL};  	
	

httpd_handle_t setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

	config.max_uri_handlers = 10;
    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &uri_main);
        httpd_register_uri_handler(server, &uri_set);
        httpd_register_uri_handler(server, &uri_dow);
        httpd_register_uri_handler(server, &uri_sav);
        httpd_register_uri_handler(server, &uri_res);
        httpd_register_uri_handler(server, &uri_men);
		httpd_register_uri_handler(server, &uri_onl);
		httpd_register_uri_handler(server, &uri_frm);
		httpd_register_uri_handler(server, &uri_cln);
		httpd_register_uri_handler(server, &uri_chr);
    }
    ESP_LOGI(TAG_WS, "Web Server is up and running\n");
    return server;
}

/*---------------------------------------------------------------------------------------------------------------------------------------*/



void app_main(void)
{   
	nvs_init();
	nvs_setup();
	memory_init();
	wifi_init();
	thingspeak_init();
	DHT_init();
	sntp_custom_init();
    setup_server();
}