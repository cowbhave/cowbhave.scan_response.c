#include "application_config.h"
#include "ruuvi_boards.h"
#include "ruuvi_driver_error.h"
#include "ruuvi_driver_sensor.h"
#include "ruuvi_interface_acceleration.h"
#include "ruuvi_interface_communication.h"
#include "ruuvi_interface_gpio.h"
#include "ruuvi_interface_gpio_interrupt.h"
#include "ruuvi_interface_lis2dh12.h"
#include "ruuvi_interface_log.h"
#include "ruuvi_interface_rtc.h"
#include "ruuvi_interface_scheduler.h"
#include "ruuvi_interface_timer.h"
#include "ruuvi_interface_yield.h"
#include "task_acceleration.h"
#include "task_gatt.h"
#include "task_led.h"
#include "ruuvi_interface_lis2dh12.h"
#include "lis2dh12_reg.h"
#include "ruuvi_interface_communication_ble4_gatt.h"


#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

RUUVI_PLATFORM_TIMER_ID_DEF(acceleration_timer);
static ruuvi_driver_sensor_t acceleration_sensor = {0};
static uint8_t m_nbr_movements;
uint16_t msg_count = 0; 


//handler for scheduled accelerometer event
static void task_acceleration_scheduler_task(void *p_event_data, uint16_t event_size)
{
  // No action necessary
}

// Timer callback, schedule accelerometer event here.
static void task_acceleration_timer_cb(void* p_context)
{
  ruuvi_platform_scheduler_event_put(NULL, 0, task_acceleration_scheduler_task);
}

static void task_acceleration_fifo_full_task(void *p_event_data, uint16_t event_size)
{
  axis3bit16_t acc;
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  
  //ruuvi_interface_acceleration_data_t data[10];
  //axis3bit16_t data[10]; //Changed watermark level in ruuvi.drivers.c\interfaces\acceleration\ruuvi_interface_lis2dh12.c
  //size_t data_len = sizeof(data);
  //err_code |= ruuvi_interface_lis2dh12_fifo_read(&data_len, data);
  uint8_t elements = 0;
  err_code |= cowbhave_fifo_data_level_get(&elements);
  char msg[APPLICATION_LOG_BUFFER_SIZE] = { 0 };
  //snprintf(msg, sizeof(msg), "%lu: Read %u data points\r\n", (uint32_t)ruuvi_platform_rtc_millis(), data_len);
  snprintf(msg, sizeof(msg), "FIFO level at %u %\r\n", elements);
  ruuvi_platform_log(RUUVI_INTERFACE_LOG_INFO, msg);
  
  //Clear "extra samples" from buffer, this should only occur during startup if sample rate > 10
  if (elements > 10){
    for (int k = 0; k < (elements-10); k++) 
      err_code |= cowbhave_acceleration_raw_get(acc.u8bit);
  }

  uint8_t lsbx;
  uint8_t lsby;
  uint8_t lsbz;
  uint8_t ii5 = 0;
  //Arrays for advertising and scan response data 
  uint8_t new_data[24];
  uint8_t new_rsp_data[24];
  memset(new_data, 0, 24);
  memset(new_rsp_data, 0, 24);
  
  //Read elements+1 when watermark is reached
  for(int ii = 0; ii < 10; ii++){
    err_code |= cowbhave_acceleration_raw_get(acc.u8bit);
    // First 5 samples to advertising data and last 5 to scan response data
    // Sometimes ~2% in quick test scan response is lost when advertising packet is received.
    // We could send e.g. even samples in advertising and odd in response
    if (ii < 5){
      new_data[ii*4 + 0] = acc.u8bit[1];
      new_data[ii*4 + 1] = acc.u8bit[3];
      new_data[ii*4 + 2] = acc.u8bit[5];
      lsbx = acc.u8bit[0];
      lsby = acc.u8bit[2] >> 2;
      lsbz = acc.u8bit[4] >> 4;
      new_data[ii*4 + 3] = lsbx | lsby | lsbz;
    } 
    else {
      ii5 = (ii-5);
      new_rsp_data[ii5*4 + 0] = acc.u8bit[1];
      new_rsp_data[ii5*4 + 1] = acc.u8bit[3];
      new_rsp_data[ii5*4 + 2] = acc.u8bit[5];
      lsbx = acc.u8bit[0];
      lsby = acc.u8bit[2] >> 2;
      lsbz = acc.u8bit[4] >> 4;
      new_rsp_data[ii5*4 + 3] = lsbx | lsby | lsbz;
    }
    
    //snprintf(msg, sizeof(msg),"%i: %u;%u;%u,%u;%u;%u\r\n", ii, acc.u8bit[0], acc.u8bit[1], acc.u8bit[2], acc.u8bit[3], acc.u8bit[4], acc.u8bit[5]);
    //ruuvi_platform_log(RUUVI_INTERFACE_LOG_INFO, msg);
    snprintf(msg, sizeof(msg),"%i %i: %i;%i;%i\r\n", msg_count, ii, acc.i16bit[0], acc.i16bit[1], acc.i16bit[2]);
    ruuvi_platform_log(RUUVI_INTERFACE_LOG_INFO, msg);
  }
  
  //Differentiate between scan response and advertising 
  //new_data[20] = 1; 
  //new_rsp_data[20] = 2;
  new_data[20] = elements; //accelerometer buffer level when task was called
  new_rsp_data[20] = elements; 

  //message counter
  new_data[21] = (msg_count >> 8);
  new_data[22] = msg_count & 0xff;
  new_rsp_data[21] = msg_count >> 8;
  new_rsp_data[22] = msg_count & 0xff;
  msg_count++;

  //TODO check for errors on data_set, err_code not the same as for acceleration get 
  //err_code |= 
  cowbhave_ble4_advertising_data_set(&new_data, &new_rsp_data, sizeof(new_data));  
  if(RUUVI_DRIVER_SUCCESS == err_code) { ruuvi_interface_watchdog_feed(); }
  RUUVI_DRIVER_ERROR_CHECK(err_code, RUUVI_DRIVER_SUCCESS);
  ruuvi_platform_yield();
}

static void on_fifo (ruuvi_interface_gpio_evt_t event)
{
  ruuvi_platform_scheduler_event_put(NULL, 0, task_acceleration_fifo_full_task);
}


static void on_movement (ruuvi_interface_gpio_evt_t event)
{
  ruuvi_platform_log(RUUVI_INTERFACE_LOG_DEBUG, "Activity\r\n");
  m_nbr_movements++;
}


static ruuvi_driver_status_t task_acceleration_configure(void)
{
  ruuvi_driver_sensor_configuration_t config;
  config.samplerate    = APPLICATION_ACCELEROMETER_SAMPLERATE;
  config.resolution    = APPLICATION_ACCELEROMETER_RESOLUTION;
  config.scale         = APPLICATION_ACCELEROMETER_SCALE;
  config.dsp_function  = APPLICATION_ACCELEROMETER_DSPFUNC;
  config.dsp_parameter = APPLICATION_ACCELEROMETER_DSPPARAM;
  config.mode          = APPLICATION_ACCELEROMETER_MODE;
  if(NULL == acceleration_sensor.data_get) { return RUUVI_DRIVER_ERROR_INVALID_STATE; }
  return acceleration_sensor.configuration_set(&acceleration_sensor, &config);
}

ruuvi_driver_status_t task_acceleration_init(void)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  ruuvi_driver_bus_t bus = RUUVI_DRIVER_BUS_NONE;
  uint8_t handle = 0;
  m_nbr_movements = 0;

  // Initialize timer for accelerometer task. Note: the timer is not started.
 err_code |= ruuvi_platform_timer_create(&acceleration_timer, RUUVI_INTERFACE_TIMER_MODE_REPEATED, task_acceleration_timer_cb);

  #if RUUVI_BOARD_ACCELEROMETER_LIS2DH12_PRESENT
    err_code = RUUVI_DRIVER_SUCCESS;
    // Only SPI supported for now
    bus = RUUVI_DRIVER_BUS_SPI;
    handle = RUUVI_BOARD_SPI_SS_ACCELEROMETER_PIN;
    err_code |= ruuvi_interface_lis2dh12_init(&acceleration_sensor, bus, handle);
    RUUVI_DRIVER_ERROR_CHECK(err_code, RUUVI_DRIVER_ERROR_NOT_FOUND);

    if(RUUVI_DRIVER_SUCCESS == err_code)
    {
      err_code |= task_acceleration_configure();

      //float ths = APPLICATION_ACCELEROMETER_ACTIVITY_THRESHOLD;
      //err_code |= ruuvi_interface_lis2dh12_activity_interrupt_use(true, &ths);

      // Let pins settle
      ruuvi_platform_delay_ms(10);
      // Setup FIFO and activity interrupts
      err_code |= ruuvi_platform_gpio_interrupt_enable(RUUVI_BOARD_INT_ACC1_PIN, RUUVI_INTERFACE_GPIO_SLOPE_LOTOHI, RUUVI_INTERFACE_GPIO_MODE_INPUT_NOPULL, on_fifo);
      //err_code |= ruuvi_platform_gpio_interrupt_enable(RUUVI_BOARD_INT_ACC2_PIN, RUUVI_INTERFACE_GPIO_SLOPE_LOTOHI, RUUVI_INTERFACE_GPIO_MODE_INPUT_NOPULL, on_movement);
      char msg[APPLICATION_LOG_BUFFER_SIZE] = { 0 };
      //snprintf(msg, sizeof(msg), "Configured interrupt threshold at %.3f mg\r\n", ths);
      ruuvi_platform_log(RUUVI_INTERFACE_LOG_INFO, msg);

      return err_code;
    }
  #endif

  // Return error if usable acceleration sensor was not found.
  return RUUVI_DRIVER_ERROR_NOT_FOUND;
}

ruuvi_driver_status_t task_acceleration_data_log(const ruuvi_interface_log_severity_t level)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  ruuvi_interface_acceleration_data_t data;
  if(NULL == acceleration_sensor.data_get) { return RUUVI_DRIVER_ERROR_INVALID_STATE; }

  err_code |= acceleration_sensor.data_get(&data);
  char message[128] = {0};
  snprintf(message, sizeof(message), "Time: %lu\r\n", (uint32_t)(data.timestamp_ms&0xFFFFFFFF));
  ruuvi_platform_log(level, message);
  snprintf(message, sizeof(message), "X: %.3f\r\n", data.x_g);
  ruuvi_platform_log(level, message);
  snprintf(message, sizeof(message), "Y: %.3f\r\n" ,data.y_g);
  ruuvi_platform_log(level, message);
  snprintf(message, sizeof(message), "Z: %.3f\r\n", data.z_g);
  ruuvi_platform_log(level, message);
  return err_code;
}

ruuvi_driver_status_t task_acceleration_data_get(ruuvi_interface_acceleration_data_t* const data)
{
  if(NULL == data) { return RUUVI_DRIVER_ERROR_NULL; }
  if(NULL == acceleration_sensor.data_get) { return RUUVI_DRIVER_ERROR_INVALID_STATE; }
  return acceleration_sensor.data_get(data);
}

ruuvi_driver_status_t task_acceleration_on_button(void)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  err_code |= task_acceleration_data_log(RUUVI_INTERFACE_LOG_INFO);
  return err_code;
}

ruuvi_driver_status_t task_acceleration_movement_count_get(uint8_t * const count)
{
  *count = m_nbr_movements;
  return RUUVI_DRIVER_SUCCESS;
}

ruuvi_driver_status_t task_acceleration_fifo_use(const bool enable)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  if(true == enable)
  {
    err_code |= ruuvi_interface_lis2dh12_fifo_use(true);
    err_code |= ruuvi_interface_lis2dh12_fifo_interrupt_use(true);
  }
  if(false == enable)
  {
    err_code |= ruuvi_interface_lis2dh12_fifo_use(false);
    err_code |= ruuvi_interface_lis2dh12_fifo_interrupt_use(false);
  }
  return err_code;
}