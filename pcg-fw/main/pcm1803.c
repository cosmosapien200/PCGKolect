#include "pcm1803.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "PCM1803";

static i2s_chan_handle_t s_rx = NULL;

esp_err_t pcm1803_init(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_AUTO,
            I2S_ROLE_SLAVE
        );
        
    chan_cfg.dma_frame_num = 480;
    chan_cfg.dma_desc_num = 2;
    printf("dma_frame_num=%lu\n", chan_cfg.dma_frame_num);
    printf("dma_desc_num=%lu\n", chan_cfg.dma_desc_num);
    ESP_ERROR_CHECK(
        i2s_new_channel(
            &chan_cfg,
            NULL,
            &s_rx
        )
        );

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = 48000,
            .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_512,
        },
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_32BIT,
                I2S_SLOT_MODE_STEREO
            ),

        .gpio_cfg = {
            .mclk = GPIO_NUM_1,
            .bclk = GPIO_NUM_2,
            .ws   = GPIO_NUM_4,
            .din  = GPIO_NUM_5,
            .dout = I2S_GPIO_UNUSED,
        },
    };

    ESP_ERROR_CHECK(
        i2s_channel_init_std_mode(
            s_rx,
            &std_cfg
        )
    );

    ESP_ERROR_CHECK(
        i2s_channel_enable(s_rx)
    );

    ESP_LOGI(TAG, "PCM1803 initialized");

    return ESP_OK;
}

i2s_chan_handle_t pcm1803_get_rx_handle(void)
{
    return s_rx;
}

esp_err_t pcm1803_read(
    void *buffer,
    size_t size,
    size_t *bytes_read
)
{
    
    return i2s_channel_read(
        s_rx,
        buffer,
        size,
        bytes_read,
        portMAX_DELAY
    );
}