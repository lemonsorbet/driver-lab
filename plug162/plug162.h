#ifndef _PLUG162_
#define _PLUG162_

#define LED_OFF   0x01
#define LED_ON    0x02

#define BUTTON_DOWN 0x01
#define BUTTON_RELEASE_TIME 100 /* 100ms */

struct plug162_device_setup {
    uint8_t                 intf_number;

    USB_Endpoint_Table_t    out_led_ep;
    USB_Endpoint_Table_t    in_button_ep;
};

void SetupHardware(void);


#endif
