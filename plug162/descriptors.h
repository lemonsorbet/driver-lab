#ifndef _DESCRIPTORS_H_
#define _DESCRIPTORS_H_

#include <avr/pgmspace.h>
#include <LUFA/Drivers/USB/USB.h>

#define INTERFACE_NUMBER 0x00

#define OUT_LED_EP_ADDR  (ENDPOINT_DIR_OUT | 1)
#define IN_BUTTON_EP_ADDR (ENDPOINT_DIR_IN | 2)

#define OUT_LED_EP_SIZE 8
#define IN_BUTTON_EP_SIZE 8

#define OUT_LED_EP_POLL 10
#define IN_BUTTON_EP_POLL 10

struct USB_Descriptor_Configuration {
    USB_Descriptor_Configuration_Header_t   conf;
    
    USB_Descriptor_Interface_t              intf;
    USB_Descriptor_Endpoint_t               out_led_ep;
    USB_Descriptor_Endpoint_t               in_button_ep;
};

    uint16_t CALLBACK_USB_GetDescriptor(const uint16_t wValue,
                                        const uint8_t wIndex,
                                        const void** const DescriptorAddress)
                                        ATTR_WARN_UNUSED_RESULT ATTR_NON_NULL_PTR_ARG(3);


#endif
