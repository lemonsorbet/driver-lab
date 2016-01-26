#include <avr/io.h>
#include <stdint.h>
#include <util/delay.h>

#include <LUFA/Drivers/Board/Buttons.h> 
#include <LUFA/Drivers/Board/LEDs.h>
#include <LUFA/Drivers/USB/USB.h>

#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/power.h>
#include <avr/interrupt.h>

#include "plug162.h"
#include "descriptors.h"

struct plug162_device_setup dev = {
    .intf_number        = INTERFACE_NUMBER,

    .out_led_ep         = {
                            .Address = OUT_LED_EP_ADDR,
                            .Size    = OUT_LED_EP_SIZE,
                            .Type    = EP_TYPE_INTERRUPT,
                            .Banks   = 1
                          },
    .in_button_ep       = {
                            .Address = IN_BUTTON_EP_ADDR,
                            .Size    = IN_BUTTON_EP_SIZE,
                            .Type     = EP_TYPE_INTERRUPT,
                            .Banks   = 1
                          }
};

bool button_down_prev = false;
uint16_t in_ep_remain_ms = 0;
uint16_t button_remain_ms = 0; 

void EVENT_USB_Device_ControlRequest(void)
{
}

void EVENT_USB_Device_Connect(void)
{
}

void EVENT_USB_Device_Disconnect(void)
{
}

void EVENT_USB_Device_ConfigurationChanged(void)
{
    Endpoint_ConfigureEndpointTable(&dev.out_led_ep, 1);
    Endpoint_ConfigureEndpointTable(&dev.in_button_ep, 1);
}

void EVENT_USB_Device_StartOfFrame(void)
{
    if (in_ep_remain_ms)
        in_ep_remain_ms--;
    if (button_remain_ms)
        button_remain_ms--;
}

void SetupHardware(void)
{
    /* Disable watchdog if enabled by bootloader/fuses */
    MCUSR &= ~(1 << WDRF);
    wdt_disable();

    /* Hardware Initialization */
    LEDs_Init();
    Buttons_Init();
    USB_Init();
    USB_Device_EnableSOFEvents();
}


static register_button_down()
{
    if (Endpoint_BytesInEndpoint())
        Endpoint_AbortPendingIN();

    if (Endpoint_IsReadWriteAllowed()) {
        Endpoint_Write_8(BUTTON_DOWN);
        Endpoint_ClearIN();
        in_ep_remain_ms = IN_BUTTON_EP_POLL;
    }
}

void plug162_do_work(void)
{
    uint8_t button_down;

    if (USB_DeviceState != DEVICE_STATE_Configured)
        return;
    
    Endpoint_SelectEndpoint(dev.out_led_ep.Address);
    
    if (Endpoint_IsReadWriteAllowed()) {
        uint8_t in_data = 0;

        in_data = Endpoint_Read_8();
        Endpoint_ClearOUT();

        switch (in_data) {
        case LED_OFF:
            LEDs_TurnOffLEDs((1 << 4));
            break;
        case LED_ON:
            LEDs_TurnOnLEDs((1 << 4));
            break;
        default:
            break;
        }
    }
   
    Endpoint_SelectEndpoint(dev.in_button_ep.Address);

    if (Endpoint_BytesInEndpoint() && !in_ep_remain_ms)
        Endpoint_AbortPendingIN(); 

    button_down = Buttons_GetStatus();
    if (!button_down && button_down_prev) {
        button_down_prev = false;
        button_remain_ms = BUTTON_RELEASE_TIME;
    } else if (button_down && !button_down_prev) {
        button_down_prev = true;
        if(!button_remain_ms)
            register_button_down();
    }
}
            
int main(void)
{
    _delay_ms(1000);
    SetupHardware();
    
    sei();
    for(;;) {
        plug162_do_work(); 
        USB_USBTask();
    }

    return 0;
}
