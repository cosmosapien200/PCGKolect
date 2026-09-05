#include "usb_device_uac.h"
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
// #include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "string.h"

#define SAMPLE_RATE 48000
#define TAG "PSRAM TEST"

static i2s_chan_handle_t rx;
int32_t* audio_dump_buffer;
int32_t buf_ptr;
#define BUFF_SIZE sizeof(int32_t)*256*44*1

#define RB_SAMPLES 8192

static int16_t *ring_buffer;
static volatile uint32_t rb_head = 0;
static volatile uint32_t rb_tail = 0;


/* =========================
   DUMMY / REQUIRED CALLBACKS
   ========================= */



static esp_err_t usb_uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    // MIC ONLY DEVICE:
    // PC is sending audio, but we ignore it completely

    (void)buf;
    (void)len;
    (void)arg;

    return ESP_OK;
}

static void usb_uac_device_set_mute_cb(uint32_t mute, void *arg)
{
    (void)arg;
    (void)mute;
    // optional: store if you later want gain control
}

static void usb_uac_device_set_volume_cb(uint32_t volume, void *arg)
{
    (void)arg;
    (void)volume;
    // optional: ignore or implement gain scaling later
}
int usb_cb_count=0;
static esp_err_t usb_uac_device_input_cb(uint8_t *buf,size_t len,size_t *bytes_read,void *arg){

    if (!rx) {
        return ESP_FAIL;
    }
    usb_cb_count+=1;
    // USB expects int16 samples
    int16_t *out = (int16_t *)buf;

    // temp buffer for I2S (32-bit samples)
    int32_t i2s_buf[256];

    size_t i2s_bytes = 0;

    esp_err_t ret = i2s_channel_read(
        rx,
        i2s_buf,
        sizeof(i2s_buf),
        &i2s_bytes,
        portMAX_DELAY
    );

    if (ret != ESP_OK || i2s_bytes == 0) {
        memset(buf, 0, len);
        *bytes_read = len;
        return ESP_OK;
    }

    int samples = i2s_bytes / sizeof(int32_t);
    int out_samples = len / sizeof(int16_t);
    // printf("len: %d",len);
    if (samples > out_samples) {
        samples = out_samples;
    }

    // memcpy(audio_dump_buffer+buf_ptr,i2s_buf,256*sizeof(int32_t));
    // buf_ptr+=256*sizeof(int32_t);
    
    for (int i = 0; i < samples; i++) {

        // PCM1803A gives 24-bit valid data inside 32-bit slot
        // Step 2: downconvert to 16-bit PCM (keep MSBs)
        out[i] = (int16_t)(i2s_buf[i] >> 16);
    }
    // printf("%d\n%d\n%d\n%d\n%d\n%d\n%d\n%d\n",out[0],out[4],out[8],out[12],out[16],out[20],out[24],out[28]);

    *bytes_read = samples * sizeof(int16_t);

    return ESP_OK;
}

// static esp_err_t usb_uac_device_input_cb(
//     uint8_t *buf,
//     size_t len,
//     size_t *bytes_read,
//     void *arg)
// {
//     int16_t *out = (int16_t *)buf;

//     int requested =
//         len / sizeof(int16_t);

//     int copied = 0;

//     while (copied < requested)
//     {
//         if (rb_tail == rb_head)
//         {
//             break;
//         }

//         out[copied++] =
//             ring_buffer[rb_tail];

//         rb_tail =
//             (rb_tail + 1) % RB_SAMPLES;
//     }

//     if (copied < requested)
//     {
//         memset(
//             out + copied,
//             0,
//             (requested - copied) *
//                 sizeof(int16_t));
//     }

//     *bytes_read =
//         requested * sizeof(int16_t);

//     return ESP_OK;
// }
/* =========================
   USB UAC INIT
   ========================= */

static void usb_uac_device_init(void)
{
    uac_device_config_t config = {
        .output_cb = usb_uac_device_output_cb,
        .input_cb = usb_uac_device_input_cb,
        .set_mute_cb = usb_uac_device_set_mute_cb,
        .set_volume_cb = usb_uac_device_set_volume_cb,
        .cb_ctx = NULL,
    };

    ESP_ERROR_CHECK(uac_device_init(&config));
}

/* =========================
   MIC INIT (PDM)
   ========================= */
void mic_i2s_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_AUTO,
            I2S_ROLE_SLAVE   // IMPORTANT
        );

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),

        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,   // IMPORTANT for USB audio simplicity
            I2S_SLOT_MODE_STEREO
        ),

        .gpio_cfg = {
            // ESP32 -> PCM1803A
            .mclk = GPIO_NUM_1,
            // PCM1803A -> ESP32
            .bclk = GPIO_NUM_2,
            .ws   = GPIO_NUM_4,
            .din  = GPIO_NUM_5,
            .dout = I2S_GPIO_UNUSED,
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
}


/* =========================
   MAIN
   ========================= */
void audio_read(void)
{
    static int32_t buffer[512];
    size_t bytes_read = 0;

    ESP_ERROR_CHECK(
        i2s_channel_read(
            rx,
            buffer,
            sizeof(buffer),
            &bytes_read,
            portMAX_DELAY
        )
    );

    // printf("Received %u bytes\n", (unsigned)bytes_read);
    for(int i=0;i<512;i+=64){
        // printf("%10ld, %10ld\n",(long signed)(buffer[i]>>8),(long signed)buffer[i+1]>>8);
        printf("%10ld\n",(long signed)buffer[i]>>8);
    }
}


static void audio_capture_task(void *arg)
{
    int32_t i2s_buf[256];
    size_t bytes_read;

    while (1)
    {
        if (i2s_channel_read(
                rx,
                i2s_buf,
                sizeof(i2s_buf),
                &bytes_read,
                portMAX_DELAY) == ESP_OK)
        {
            int samples = bytes_read / sizeof(int32_t);

            for (int i = 0; i < samples; i++)
            {
                uint32_t next =
                    (rb_head + 1) % RB_SAMPLES;

                if (next != rb_tail)
                {
                    ring_buffer[rb_head] =
                        (int16_t)(i2s_buf[i] >> 16);

                    rb_head = next;
                }
                else
                {
                    // overflow
                }
            }
        }
    }
}

void app_main(void)
{
    // size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    // size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    
    // ESP_LOGI(TAG, "Total PSRAM size: %d bytes", total_psram);
    // ESP_LOGI(TAG, "Free PSRAM size: %d bytes", free_psram);
    // if (total_psram == 0) {
    //     ESP_LOGE(TAG, "PSRAM not detected or not enabled in menuconfig!");
    //     return;
    // }
    audio_dump_buffer = (int32_t*) malloc(BUFF_SIZE);
    buf_ptr=0;
    mic_i2s_init();
    ring_buffer = heap_caps_malloc(
        RB_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_SPIRAM
    );
    usb_uac_device_init();
    while (1) {
        // audio_read();
        // if(buf_ptr>=BUFF_SIZE-256*sizeof(uint32_t)){
        //     buf_ptr=0;
        //     for(int i=0;i<BUFF_SIZE;i++){
        //         printf("%ld,",audio_dump_buffer[i]);
        //     }
        //     printf("\n");
        // }
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf("USB CB COUNT: %d\n", usb_cb_count);
            usb_cb_count=0;
    }
}