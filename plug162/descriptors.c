#include "descriptors.h"

const USB_Descriptor_Device_t PROGMEM dev_desc = {
    .Header                     = {.Size = sizeof(USB_Descriptor_Device_t),
                                    .Type = DTYPE_Device},
    
    .USBSpecification           = VERSION_BCD(02.00),
    .Class                      = USB_CSCP_VendorSpecificClass,
    .SubClass                   = USB_CSCP_VendorSpecificSubclass,
    .Protocol                   = USB_CSCP_VendorSpecificProtocol,
    .Endpoint0Size              = FIXED_CONTROL_ENDPOINT_SIZE,

    .VendorID                   = 0xdead,
    .ProductID                  = 0xbeef,
    .ReleaseNumber              = VERSION_BCD(00.01),
    .ManufacturerStrIndex       = 0x01,
    .ProductStrIndex            = 0x02,
    .SerialNumStrIndex          = USE_INTERNAL_SERIAL,

    .NumberOfConfigurations     = FIXED_NUM_CONFIGURATIONS
};


const struct USB_Descriptor_Configuration PROGMEM conf_desc = {
    .conf = {
        .Header                 = 
                {.Size = sizeof(USB_Descriptor_Configuration_Header_t),
                 .Type = DTYPE_Configuration},
        
        .TotalConfigurationSize = sizeof(struct USB_Descriptor_Configuration),
        .TotalInterfaces        = 1,
        .ConfigurationNumber    = 1,
        .ConfigurationStrIndex  = NO_DESCRIPTOR,
        .ConfigAttributes       = USB_CONFIG_ATTR_RESERVED, 
        .MaxPowerConsumption    = USB_CONFIG_POWER_MA(100),
    },
    
    .intf = {
        .Header                 = {.Size = sizeof(USB_Descriptor_Interface_t),
                                   .Type = DTYPE_Interface},
        
        .InterfaceNumber        = INTERFACE_NUMBER,
        .AlternateSetting       = 0x00,
        
        .TotalEndpoints         = 2,

        .Class                  = USB_CSCP_VendorSpecificClass,
        .SubClass               = USB_CSCP_VendorSpecificSubclass,
        .Protocol               = USB_CSCP_VendorSpecificProtocol,

        .InterfaceStrIndex      = NO_DESCRIPTOR
    },

    .out_led_ep = {
        .Header                 = {.Size = sizeof(USB_Descriptor_Endpoint_t),
                                   .Type = DTYPE_Endpoint},
        .EndpointAddress        = OUT_LED_EP_ADDR,
        .Attributes             = EP_TYPE_INTERRUPT | ENDPOINT_ATTR_NO_SYNC |
                                  ENDPOINT_USAGE_DATA,
        .EndpointSize           = OUT_LED_EP_SIZE,
        .PollingIntervalMS      = OUT_LED_EP_POLL
    },

    .in_button_ep = {
        .Header                 = {.Size = sizeof(USB_Descriptor_Endpoint_t),
                                   .Type = DTYPE_Endpoint},
        .EndpointAddress        = IN_BUTTON_EP_ADDR,
        .Attributes             = EP_TYPE_INTERRUPT | ENDPOINT_ATTR_NO_SYNC |
                                  ENDPOINT_USAGE_DATA,
        .EndpointSize           = IN_BUTTON_EP_SIZE,
        .PollingIntervalMS      = IN_BUTTON_EP_POLL
    }
};

const USB_Descriptor_String_t PROGMEM LanguageString =
{
    .Header                 = {.Size = USB_STRING_LEN(1), .Type = DTYPE_String},

    .UnicodeString          = {LANGUAGE_ID_ENG}
};

const USB_Descriptor_String_t PROGMEM ManufacturerString =
{
    .Header                 = {.Size = USB_STRING_LEN(13), .Type = DTYPE_String},

    .UnicodeString          = L"Fredrik Yhlen"
};

const USB_Descriptor_String_t PROGMEM ProductString =
{
    .Header                 = {.Size = USB_STRING_LEN(11), .Type = DTYPE_String},

    .UnicodeString          = L"USB-PLUG162"
};

uint16_t CALLBACK_USB_GetDescriptor(const uint16_t wValue,
                                    const uint8_t wIndex,
                                    const void** const DescriptorAddress)
{
    const uint8_t  DescriptorType   = (wValue >> 8);
    const uint8_t  DescriptorNumber = (wValue & 0xFF);

    const void* Address = NULL;
    uint16_t    Size    = NO_DESCRIPTOR;

    switch (DescriptorType)
    {
        case DTYPE_Device:
            Address = &dev_desc;
            Size    = sizeof(USB_Descriptor_Device_t);
            break;
        case DTYPE_Configuration:
            Address = &conf_desc;
            Size    = sizeof(struct USB_Descriptor_Configuration);
            break;
        case DTYPE_String:
            switch (DescriptorNumber)
            {
                case 0x00:
                    Address = &LanguageString;
                    Size    = pgm_read_byte(&LanguageString.Header.Size);
                    break;
                case 0x01:
                    Address = &ManufacturerString;
                    Size    = pgm_read_byte(&ManufacturerString.Header.Size);
                    break;
                case 0x02:
                    Address = &ProductString;
                    Size    = pgm_read_byte(&ProductString.Header.Size);
                    break;
            }

                 break;
    }

    *DescriptorAddress = Address;
    return Size;
}




