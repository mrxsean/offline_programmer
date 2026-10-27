#pragma once
#include "mock_idf.h"
#define ESP_ERR_NOT_SUPPORTED 9
#define GPIO_MODE_INPUT_OUTPUT 3
#define GPIO_MODE_INPUT_OUTPUT_OD 7
#define SPI2_HOST 1
#define SPI_DMA_DISABLED 0
typedef void *spi_device_handle_t;
typedef struct { int sclk_io_num, mosi_io_num, miso_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num, queue_size; } spi_device_interface_config_t;
typedef struct { size_t length; const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
esp_err_t spi_bus_initialize(int, const spi_bus_config_t *, int);
esp_err_t spi_bus_add_device(int, const spi_device_interface_config_t *, spi_device_handle_t *);
esp_err_t spi_bus_remove_device(spi_device_handle_t);
esp_err_t spi_bus_free(int);
esp_err_t spi_device_transmit(spi_device_handle_t, spi_transaction_t *);
void esp_rom_delay_us(uint32_t);
int64_t esp_timer_get_time(void);
typedef void *adc_oneshot_unit_handle_t;
typedef void *adc_cali_handle_t;
typedef int adc_channel_t;
typedef int adc_unit_t;
typedef int adc_atten_t;
#define ADC_UNIT_1 0
#define ADC_ATTEN_DB_0 0
#define ADC_ATTEN_DB_2_5 1
#define ADC_ATTEN_DB_6 2
#define ADC_ATTEN_DB_12 3
#define ADC_BITWIDTH_12 12
typedef struct { adc_unit_t unit_id; } adc_oneshot_unit_init_cfg_t;
typedef struct { adc_atten_t atten; int bitwidth; } adc_oneshot_chan_cfg_t;
typedef struct { adc_unit_t unit_id; adc_channel_t chan; adc_atten_t atten; int bitwidth; } adc_cali_curve_fitting_config_t;
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *, adc_oneshot_unit_handle_t *);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t, adc_channel_t, const adc_oneshot_chan_cfg_t *);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t);
esp_err_t adc_oneshot_io_to_channel(int, adc_unit_t *, adc_channel_t *);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t, adc_channel_t, int *);
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *, adc_cali_handle_t *);
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t);
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t, int, int *);
