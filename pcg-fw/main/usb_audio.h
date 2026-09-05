#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include "esp_err.h"

esp_err_t usb_audio_init(void);

int usb_audio_get_callback_count(void);

void usb_audio_reset_callback_count(void);

#endif