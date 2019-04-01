/**
 * Ruuvi BLE data advertising.
 *
 * License: BSD-3
 * Author: Otso Jousimaa <otso@ojousima.net>
 */

#include "ruuvi_platform_external_includes.h"
#if NRF5_SDK15_COMMUNICATION_BLE4_ADVERTISING_ENABLED
#include "ruuvi_driver_error.h"
#include "ruuvi_interface_communication.h"
#include "ruuvi_interface_communication_radio.h"
#include "ruuvi_interface_communication_ble4_advertising.h"
#include <stdint.h>

#include "nordic_common.h"
#include "nrf_nvic.h"
#include "nrf_soc.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "ble_advdata.h"
#include "ble_types.h"
#include "sdk_errors.h"
#include "ble_advertising.h"

typedef struct {
    uint32_t advertisement_interval_ms;
    int8_t advertisement_power_dbm;
    uint16_t manufacturer_id;
    ruuvi_interface_communication_t* channel;
}ruuvi_platform_ble4_advertisement_state_t;

// Buffer for advertised data - TODO: Use actual ringbuffer
//static uint8_t  m_advertisement0[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
//static uint16_t m_adv0_len;

//static uint8_t  m_advertisement1[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
//static uint16_t m_adv1_len;

//static uint8_t advert_array[24];
//static uint8_t scan_array[24];
//static uint16_t array_len; 





//static ble_gap_adv_data_t m_adv_data;


// TODO: Define somewhere else. SDK_APPLICATION_CONFIG?
#define DEFAULT_ADV_INTERVAL_MS 1010
#define MIN_ADV_INTERVAL_MS     100
#define MAX_ADV_INTERVAL_MS     10000
static ble_gap_adv_params_t   m_adv_params;                                  /**< Parameters to be passed to the stack when starting advertising. */
static uint8_t                m_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET; /**< Advertising handle used to identify an advertising set. */
static bool                   m_advertisement_is_init = false;               /**< Flag for initialization **/
static bool                   m_advertising = false;                         /**< Flag for advertising in process **/
ruuvi_platform_ble4_advertisement_state_t m_adv_state;


static bool advertisement_odd = false;
//Example for two buffers from nRF5_SDK_15.2.0_9412b96\components\softdevice\s132\doc\s132_nrf52_6.1.0_migration-document.pdf

static uint8_t raw_adv_data_buffer1[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t raw_scan_rsp_data_buffer1[BLE_GAP_ADV_SET_DATA_SIZE_MAX];

static ble_gap_adv_data_t adv_data1 = {
  .adv_data.p_data = raw_adv_data_buffer1, 
  .adv_data.len = sizeof(raw_adv_data_buffer1),
  .scan_rsp_data.p_data = raw_scan_rsp_data_buffer1, .scan_rsp_data.len = sizeof(raw_scan_rsp_data_buffer1)};
/* A second advertising data buffer for later updating advertising data
while advertising */
static uint8_t raw_adv_data_buffer2[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t raw_scan_rsp_data_buffer2[BLE_GAP_ADV_SET_DATA_SIZE_MAX];

static ble_gap_adv_data_t adv_data2 = {
  .adv_data.p_data = raw_adv_data_buffer2, 
  .adv_data.len = sizeof(raw_adv_data_buffer2),
  .scan_rsp_data.p_data = raw_scan_rsp_data_buffer2, 
  .scan_rsp_data.len = sizeof(raw_scan_rsp_data_buffer2)};

//Reused when updating data
static ble_advdata_manuf_data_t manuf_data;
static ble_advdata_manuf_data_t  manuf_data_response;
static ble_advdata_t advdata = {0};
static ble_advdata_t srdata = {0};


// Update BLE settings, takes effect immidiately
static ruuvi_driver_status_t update_settings(void)
{
    if (!m_advertisement_is_init) { return RUUVI_DRIVER_ERROR_INVALID_STATE; }
    ret_code_t err_code = NRF_SUCCESS;
    // Stop advertising for setting update
    if (m_advertising)
    {
        err_code |= sd_ble_gap_adv_stop(m_adv_handle);
    }
    err_code |= sd_ble_gap_adv_set_configure(&m_adv_handle, &adv_data1, &m_adv_params);
    if (m_advertising)
    {
        err_code = sd_ble_gap_adv_start(m_adv_handle, NRF5_SDK15_BLE4_STACK_CONN_TAG);
    }

    return ruuvi_platform_to_ruuvi_error(&err_code);
}

/*
 * Assume that radio activity was caused by this module and call event handler with sent-event
 */
void ruuvi_platform_communication_ble4_advertising_activity_handler(const ruuvi_interface_communication_radio_activity_evt_t evt)
{
  // Before activity - no action
  if(RUUVI_INTERFACE_COMMUNICATION_RADIO_BEFORE == evt ) { return; }

  // After activity - assume that all activity is related to advertisement tx
  if(RUUVI_INTERFACE_COMMUNICATION_RADIO_AFTER == evt)
  {
    if(NULL != m_adv_state.channel->on_evt)
    {
      // TODO: Add information about sent advertisement
      m_adv_state.channel->on_evt(RUUVI_INTERFACE_COMMUNICATION_SENT, NULL, 0);
    }
  }
}

ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_tx_interval_set(const uint32_t ms)
{
    if (MIN_ADV_INTERVAL_MS > ms || MAX_ADV_INTERVAL_MS < ms) { return RUUVI_DRIVER_ERROR_INVALID_PARAM; }
    m_adv_state.advertisement_interval_ms = ms;
    m_adv_params.interval = MSEC_TO_UNITS(m_adv_state.advertisement_interval_ms, UNIT_0_625_MS);
    return update_settings();
}

ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_tx_interval_get(uint32_t* ms)
{
  *ms = m_adv_state.advertisement_interval_ms;
  return RUUVI_DRIVER_SUCCESS;
}

ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_manufacturer_id_set(const uint16_t id)
{
  m_adv_state.manufacturer_id = id;
  return RUUVI_DRIVER_SUCCESS;
}

/*
 * Initializes radio hardware, advertising module and scanning module
 *
 * Returns RUUVI_DIRVER_SUCCESS on success, RUUVI_DIRVER_ERROR_INVALID_STATE if radio is already initialized
 */
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_init(ruuvi_interface_communication_t* const channel)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  if (!m_advertisement_is_init)
  {
     err_code |= ruuvi_interface_communication_radio_init(RUUVI_INTERFACE_COMMUNICATION_RADIO_ADVERTISEMENT);
    if(RUUVI_DRIVER_SUCCESS != err_code) { return err_code; }
  }

  // Initialize advertising parameters (used when starting advertising).
  memset(&m_adv_params, 0, sizeof(m_adv_params));
  m_adv_params.filter_policy   = BLE_GAP_ADV_FP_ANY;
  m_adv_params.duration        = 0;       // Never time out.
  m_adv_params.properties.type = BLE_GAP_ADV_TYPE_NONCONNECTABLE_SCANNABLE_UNDIRECTED; //BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
  m_adv_params.p_peer_addr     = NULL;    // Undirected advertisement.
  m_adv_params.interval        = MSEC_TO_UNITS(DEFAULT_ADV_INTERVAL_MS, UNIT_0_625_MS);
  //Advertise only on certain channels -> limiting channels leads to fewer samples received on RPi
  //m_adv_params.channel_mask[4] = 0xc0; //Only 37
  //m_adv_params.channel_mask[4] = 0x80; //Not 39
  //m_adv_params.channel_mask[4] = 0xa0; //Only 38 

    m_advertisement_is_init = true;
    m_adv_state.advertisement_interval_ms = DEFAULT_ADV_INTERVAL_MS;
    m_adv_state.channel = channel;
    channel->init    = ruuvi_interface_communication_ble4_advertising_init;
    channel->uninit  = ruuvi_interface_communication_ble4_advertising_uninit;
    channel->send    = ruuvi_interface_communication_ble4_advertising_send;
    channel->read    = ruuvi_interface_communication_ble4_advertising_receive;
    channel->on_evt  = NULL;
     

   //Set manufacturing data
    memset(&manuf_data, 0, sizeof(manuf_data));
    uint8_t data[24]                          = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
    manuf_data.company_identifier             =  COWBHAVE_MANUFACTURER_ID;
    manuf_data.data.p_data                    = data;
    manuf_data.data.size                      = sizeof(data);
    advdata.p_manuf_specific_data = &manuf_data;
    advdata.name_type               = BLE_ADVDATA_NO_NAME;
    advdata.short_name_len = 0; // Advertise only first 6 letters of name
    advdata.include_appearance      = false;
    advdata.uuids_complete.uuid_cnt = 0; //sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
 
    // Prepare the scan response manufacturer specific data packet   
    uint8_t                     data_response[24] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
    manuf_data_response.company_identifier  = COWBHAVE_MANUFACTURER_ID;
    manuf_data_response.data.p_data         = data_response;
    manuf_data_response.data.size           = sizeof(data_response);
    srdata.name_type = BLE_ADVDATA_NO_NAME;
    srdata.short_name_len = 0;
    srdata.p_manuf_specific_data = &manuf_data_response;

    ret_code_t err_code2;
    err_code2 |= ble_advdata_encode(&advdata, &adv_data1.adv_data.p_data[0], &adv_data1.adv_data.len);
    err_code2 |= ble_advdata_encode(&srdata,  &adv_data1.scan_rsp_data.p_data[0], &adv_data1.scan_rsp_data.len);
    
    return ruuvi_platform_to_ruuvi_error(&err_code);
}

/*
 * Uninitializes radio hardware, advertising module and scanning module
 *
 * Returns RUUVI_DIRVER_SUCCESS on success or if radio was not initialized.
 * Returns RUUVI_DRIVER_ERROR_INVALID_STATE if radio hardware was initialized by another radio module.
 */
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_uninit(ruuvi_interface_communication_t* const channel)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  // Stop advertising
  if(true == m_advertising)
  {
    sd_ble_gap_adv_stop(m_adv_handle);
    m_advertising = false;
  }

  // Clear advertisement parameters
  memset(&m_adv_params, 0, sizeof(m_adv_params));

  // Release radio
  err_code |= ruuvi_interface_communication_radio_uninit(RUUVI_INTERFACE_COMMUNICATION_RADIO_ADVERTISEMENT);
  m_advertisement_is_init = false;

  // Clear function pointers
  memset(channel, 0, sizeof(ruuvi_interface_communication_t));
  memset(&m_adv_state, 0 ,sizeof(m_adv_state));

  return err_code;
}


// Not implemented
//ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_rx_interval_set(uint32_t* window_interval_ms, uint32_t* window_size_ms);
//ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_rx_interval_get(uint32_t* window_interval_ms, uint32_t* window_size_ms);

// Set manufacturer specific data to advertise. Clears previous data.
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_data_set(const uint8_t* data, const uint8_t data_length)
{
  if(NULL == data)     { return RUUVI_DRIVER_ERROR_NULL; }
  if(24 < data_length) { return RUUVI_DRIVER_ERROR_INVALID_LENGTH; }

  // Build specification for data into ble_advdata_t advdata
  ble_advdata_t advdata = {0};
  
  // Build manufacturer specific data
  ble_advdata_manuf_data_t manuf_specific_data;
  ret_code_t err_code = NRF_SUCCESS;
  // Preserve const of data passed to us.
  uint8_t manufacturer_data[24];
  memcpy(manufacturer_data, data, data_length);
  manuf_specific_data.data.p_data = manufacturer_data;
  manuf_specific_data.data.size   = data_length;
  manuf_specific_data.company_identifier =  0x0059;
  advdata.p_manuf_specific_data = &manuf_specific_data;
  advdata.name_type               = BLE_ADVDATA_NO_NAME;
  advdata.short_name_len = 0; 

  //Build scan response data
  ble_advdata_t srdata = {0};
  ble_advdata_manuf_data_t  manuf_data_response;
  uint8_t data_response[24];
  memcpy(data_response, data, data_length); //Use same data for testing
  manuf_data_response.company_identifier  = 0x0059;
  manuf_data_response.data.p_data         = data_response;
  manuf_data_response.data.size           = sizeof(data_response);
  srdata.name_type = BLE_ADVDATA_NO_NAME;
  srdata.short_name_len = 0;
  srdata.p_manuf_specific_data = &manuf_data_response;
 
  if (advertisement_odd)
  {
    err_code |= ble_advdata_encode(&advdata, &adv_data2.adv_data.p_data[0], &adv_data2.adv_data.len);
    err_code |= ble_advdata_encode(&srdata,  &adv_data2.scan_rsp_data.p_data[0], &adv_data2.scan_rsp_data.len);
    err_code |= sd_ble_gap_adv_set_configure(&m_adv_handle, &adv_data2, NULL);
  } else {
    err_code |= ble_advdata_encode(&advdata, &adv_data1.adv_data.p_data[0], &adv_data1.adv_data.len);
    err_code |= ble_advdata_encode(&srdata,  &adv_data1.scan_rsp_data.p_data[0], &adv_data1.scan_rsp_data.len);
    err_code |= sd_ble_gap_adv_set_configure(&m_adv_handle, &adv_data1, NULL);
  }
  
  advertisement_odd = !advertisement_odd;
  return ruuvi_platform_to_ruuvi_error(&err_code);
}

/**
 * Send data as manufacturer specific data payload.
 * If no new data is placed to the buffer, last message sent will be repeated.
 *
 * Returns RUUVI_DRIVER_SUCCESS if the data was queued to Softdevice
 * Returns RUUVI_DRIVER_ERROR_NULL if the data was null.
 * Returns RUUVI_DRIVER_ERROR_INVALID_LENGTH if data length is over 24 bytes
 */
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_send(ruuvi_interface_communication_message_t* message)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  err_code |= ruuvi_interface_communication_ble4_advertising_data_set(message->data, message->data_length);
    // Start advertising if it was not already started
  if(false == m_advertising)
  {
    err_code |= sd_ble_gap_adv_start(m_adv_handle, NRF5_SDK15_BLE4_STACK_CONN_TAG);
    m_advertising = true;
  }

  return err_code;
}

//Add separate start command
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_start()
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  if(false == m_advertising)
  {
    err_code |= sd_ble_gap_adv_start(m_adv_handle, NRF5_SDK15_BLE4_STACK_CONN_TAG);
    m_advertising = true;
  }
  return err_code;
}

// Not implemented
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_receive(ruuvi_interface_communication_message_t* message)
{
  return RUUVI_DRIVER_ERROR_NOT_IMPLEMENTED;
}

// TODO: Device-specific TX powers
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_tx_power_set(int8_t* dbm)
{
    int8_t  tx_power = 0;
    ret_code_t err_code = NRF_SUCCESS;
    if (*dbm <= -40) { tx_power = -40; }
    else if (*dbm <= -20) { tx_power = -20; }
    else if (*dbm <= -16) { tx_power = -16; }
    else if (*dbm <= -12) { tx_power = -12; }
    else if (*dbm <= -8 ) { tx_power = -8; }
    else if (*dbm <= -4 ) { tx_power = -4; }
    else if (*dbm <= 0  ) { tx_power = 0; }
    else if (*dbm <= 4  ) { tx_power = 4; }
    else { return RUUVI_DRIVER_ERROR_INVALID_PARAM; }
    err_code = sd_ble_gap_tx_power_set (BLE_GAP_TX_POWER_ROLE_ADV,
                                        m_adv_handle,
                                        tx_power
                                       );
    return ruuvi_platform_to_ruuvi_error(&err_code);
}


ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_tx_power_get(int8_t* dbm)
{
  return RUUVI_DRIVER_ERROR_NOT_IMPLEMENTED;
}


ruuvi_driver_status_t cowbhave_ble4_advertising_data_set(const uint8_t* new_adv_data, const uint8_t* new_rsp_data, const uint8_t data_length)
{
  if(NULL == new_adv_data)     { return RUUVI_DRIVER_ERROR_NULL; }
  if(24 < data_length) { return RUUVI_DRIVER_ERROR_INVALID_LENGTH; }

  ret_code_t err_code = NRF_SUCCESS;
  // Preserve const of data passed to us.
  uint8_t manufacturer_data[24];
  memcpy(manufacturer_data, new_adv_data, data_length);
  manuf_data.data.p_data = manufacturer_data;
  manuf_data.data.size   = data_length;
  advdata.p_manuf_specific_data = &manuf_data;
  
  uint8_t data_response[24];
  memcpy(data_response, new_rsp_data, data_length); //Use same data for testing
  manuf_data_response.data.p_data         = data_response;
  manuf_data_response.data.size           = sizeof(data_response);
  srdata.p_manuf_specific_data = &manuf_data_response;
 
  if (advertisement_odd)
  {
    err_code |= ble_advdata_encode(&advdata, &adv_data2.adv_data.p_data[0], &adv_data2.adv_data.len);
    err_code |= ble_advdata_encode(&srdata,  &adv_data2.scan_rsp_data.p_data[0], &adv_data2.scan_rsp_data.len);
    err_code |= sd_ble_gap_adv_set_configure(&m_adv_handle, &adv_data2, NULL);
  } else {
    err_code |= ble_advdata_encode(&advdata, &adv_data1.adv_data.p_data[0], &adv_data1.adv_data.len);
    err_code |= ble_advdata_encode(&srdata,  &adv_data1.scan_rsp_data.p_data[0], &adv_data1.scan_rsp_data.len);
    err_code |= sd_ble_gap_adv_set_configure(&m_adv_handle, &adv_data1, NULL);
  }
  
  advertisement_odd = !advertisement_odd;
  return ruuvi_platform_to_ruuvi_error(&err_code);
}



#endif