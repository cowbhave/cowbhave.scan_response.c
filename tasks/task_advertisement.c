/**
 * Ruuvi Firmware 3.x advertisement tasks.
 *
 * License: BSD-3
 * Author: Otso Jousimaa <otso@ojousima.net>
 **/

#include "application_config.h"
#include "ruuvi_boards.h"
#include "ruuvi_driver_error.h"
//#include "ruuvi_endpoint_3.h"
//#include "ruuvi_endpoint_5.h"
#include "ruuvi_interface_acceleration.h"
#include "ruuvi_interface_adc.h"
#include "ruuvi_interface_communication_ble4_advertising.h"
#include "ruuvi_interface_communication_radio.h"
#include "ruuvi_interface_environmental.h"
#include "ruuvi_interface_scheduler.h"
#include "ruuvi_interface_timer.h"
#include "ruuvi_interface_watchdog.h"
#include "task_adc.h"
#include "task_advertisement.h"
#include "task_acceleration.h"
#include "task_environmental.h"

RUUVI_PLATFORM_TIMER_ID_DEF(advertisement_timer);
static ruuvi_interface_communication_t channel;

//handler for scheduled advertisement event
static void task_advertisement_scheduler_task(void *p_event_data, uint16_t event_size)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  // Update BLE data
  if(APPLICATION_DATA_FORMAT == 3) { err_code |= task_advertisement_send_3(); }
  if(APPLICATION_DATA_FORMAT == 5) { err_code |= task_advertisement_send_5(); }
  if(RUUVI_DRIVER_SUCCESS == err_code) { ruuvi_interface_watchdog_feed(); }
}

// Timer callback, schedule advertisement event here.
static void task_advertisement_timer_cb(void* p_context)
{
  ruuvi_platform_scheduler_event_put(NULL, 0, task_advertisement_scheduler_task);
}

ruuvi_driver_status_t task_advertisement_init(void)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  err_code |= ruuvi_interface_communication_ble4_advertising_init(&channel);
  err_code |= ruuvi_interface_communication_ble4_advertising_tx_interval_set(APPLICATION_ADVERTISING_INTERVAL);
  int8_t target_power = APPLICATION_ADVERTISING_POWER;
  err_code |= ruuvi_interface_communication_ble4_advertising_tx_power_set(&target_power);
  err_code |= ruuvi_interface_communication_ble4_advertising_manufacturer_id_set(RUUVI_BOARD_BLE_MANUFACTURER_ID);
  err_code |= ruuvi_interface_communication_ble4_advertising_start();
  //Do not start ruuvi timers at all -> would be better to create a new file with CowBhave tasks and take out the rest
  //advertising data will be update in when acc fifo is full
  //err_code |= ruuvi_platform_timer_create(&advertisement_timer, RUUVI_INTERFACE_TIMER_MODE_REPEATED, task_advertisement_timer_cb);
  //err_code |= ruuvi_platform_timer_start(advertisement_timer, APPLICATION_ADVERTISING_INTERVAL);
  return err_code;
}

