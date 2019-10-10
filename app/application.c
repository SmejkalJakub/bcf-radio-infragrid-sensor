#include <application.h>

#define INFRAGRID_UPDATE_INTERVAL (1 * 1000)
#define BATTERY_UPDATE_INTERVAL (60 * 60 * 1000)

// LED instance
bc_led_t led;

// Button instance
bc_button_t button;

bc_module_pir_t pir;

// Infra Grid Module
bc_module_infra_grid_t infra;

// 8x8 temperature array
float temperatures[64];

uint8_t timetable_index;
bool timetable_force_reload = false;

bc_tick_t pir_deadtime_timestamp = 0;

bc_scheduler_task_id_t infragrid_task_id;


float map_f(float x, float in_min, float in_max, float out_min, float out_max)
{
    int32_t val = (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;

    if (val > out_max)
    {
        val = out_max;
    }
    if (val < out_min)
    {
        val = out_min;
    }

    return val;
}

uint16_t bit_packer(uint8_t *src, uint16_t src_size, uint8_t *dst, uint16_t dst_size, uint8_t bit_count)
{
    memset(dst, 0x00, dst_size);

    uint32_t bit = 0;
    uint16_t max_dst_index = 0;
    for (int i = 0; i < src_size; i++)
    {
        uint8_t val = (uint8_t)src[i];
        uint32_t b = bit / 8;
        uint32_t offset = bit % 8;
        if (offset > 2)
        {
            max_dst_index = b + 1;
            if (max_dst_index > dst_size)
            {
                return 0;
            }
            dst[b] |= val << offset;
            dst[b+1] |= val >> (8-offset);
        }
        else
        {
            max_dst_index = b;
            if (max_dst_index > dst_size)
            {
                return 0;
            }
            dst[b] |= val << offset;
        }
        bit += bit_count;
    }

    return max_dst_index;
}

size_t compress(uint8_t *in, size_t in_size, uint8_t *buffer, size_t buffer_size)
{
    memset(buffer, 0x00, buffer_size);

    uint32_t bit = 0;
    uint8_t val;
    size_t b = 0;
    uint32_t offset;

    for(size_t i = 0; i < in_size; i++)
    {
        val = in[i];

        b = bit / 8;

        if (b + 1 > buffer_size)
        {
            return 0;
        }

        offset = bit % 8;

        if(offset > 2)
        {
            buffer[b] |= val << offset;
            buffer[b+1] |= val >> (8-offset);
        }
        else
        {
            buffer[b] |= val << offset;
        }

        bit += 6;
    }

    return b + 1;
}

size_t decompress(uint8_t *out, size_t out_size, uint8_t *buffer, size_t buffer_size)
{
    uint32_t bit = 0;
    size_t b = 0;
    uint8_t val;
    uint32_t offset;

    for(size_t i = 0; i < out_size; i++)
    {
        b = bit / 8;

        offset = bit % 8;

        if (b + 1 > buffer_size)
        {
            return 0;
        }

        if (offset > 2)
        {
            val = (buffer[b] >> offset) | (buffer[b + 1] << (8 - offset));
        }
        else
        {
            val = buffer[b] >> offset;
        }

        out[i] = val & 0x3F;

        bit += 6;
    }

    return b;
}

void bc_module_infra_grid_log(float *temperature)
{
    for (int i = 0; i < 8; i++)
    {
        bc_log_debug("%2.1f %2.1f %2.1f %2.1f %2.1f %2.1f %2.1f %2.1f", 
            temperatures[8*i + 0], temperatures[8*i + 1], temperatures[8*i + 2], temperatures[8*i + 3],
            temperatures[8*i + 4], temperatures[8*i + 5], temperatures[8*i + 6], temperatures[8*i + 7]);
    }
}

void bc_module_infra_grid_log2(float *temperature)
{
    char c[] = {' ', '.', ':', '=', '#', '%', '@'};

    float in_min = 20.0f;
    float in_max = 40.0f;

    for (int i = 0; i < 8; i++)
    {
        bc_log_debug("%c%c%c%c%c%c%c%c", 
            c[ (uint8_t)map_f(temperatures[8*i + 0], in_min, in_max, 0, sizeof(c)) ],
            c[ (uint8_t)map_f(temperatures[8*i + 1], in_min, in_max, 0, sizeof(c)) ],
            c[ (uint8_t)map_f(temperatures[8*i + 2], in_min, in_max, 0, sizeof(c)) ],
            c[ (uint8_t)map_f(temperatures[8*i + 3], in_min, in_max, 0, sizeof(c)) ],
            c[ (uint8_t)map_f(temperatures[8*i + 4], in_min, in_max, 0, sizeof(c)) ],
            c[ (uint8_t)map_f(temperatures[8*i + 5], in_min, in_max, 0, sizeof(c)) ],
            c[ (uint8_t)map_f(temperatures[8*i + 6], in_min, in_max, 0, sizeof(c)) ],
            c[ (uint8_t)map_f(temperatures[8*i + 7], in_min, in_max, 0, sizeof(c)) ]
            );
    }
}

void button_event_handler(bc_button_t *self, bc_button_event_t event, void *event_param)
{
    if (event == BC_BUTTON_EVENT_CLICK)
    {
        static uint16_t event_count = 0;
        bc_led_pulse(&led, 100);
        bc_radio_pub_push_button(&event_count);
        event_count++;
    }
}

void infragrid_event_handler(bc_module_infra_grid_t *self, bc_module_infra_grid_event_t event, void *param)
{
    if (event == BC_MODULE_INFRA_GRID_EVENT_ERROR)
    {
        bc_log_debug("INFRA ERROR");
    }

    if (event == BC_MODULE_INFRA_GRID_EVENT_UPDATE)
    {
        bc_log_debug("INFRA UPDATE");
        bc_module_infra_grid_get_temperatures_celsius(self, temperatures);
        
        bc_module_infra_grid_log(temperatures);
        bc_module_infra_grid_log2(temperatures);

        uint8_t src[64];
        uint8_t dst[50];

        for (int i = 0; i < 64; i++)
        {
            // Offset by 0°C
            int temp = (uint8_t)temperatures[i] - 0;
            if (temp < 0)
            {
                temp = 0;
            }
            src[i] = (uint8_t)temp;
        }

        //uint16_t dst_size = bit_packer(src, sizeof(src), dst, sizeof(dst), 6);
        uint16_t dst_size = compress(src, sizeof(src), dst, sizeof(dst));

        /*static volatile uint8_t dec[100];
        decompress(dec, sizeof(dec), dst, dst_size);*/

        bc_log_debug("bit_packer dst_size: %d", dst_size);

        if (dst_size)
        {
            bc_radio_pub_buffer(dst, dst_size);
        }
    }
}

void battery_event_handler(bc_module_battery_event_t event, void *event_param)
{
    (void) event_param;

    float voltage;

    if (event == BC_MODULE_BATTERY_EVENT_UPDATE)
    {
        if (bc_module_battery_get_voltage(&voltage))
        {
            bc_radio_pub_battery(&voltage);
        }
    }
}


void application_init(void)
{
    // Initialize logging
    bc_log_init(BC_LOG_LEVEL_DUMP, BC_LOG_TIMESTAMP_ABS);

    // Initialize LED
    bc_led_init(&led, BC_GPIO_LED, false, false);
    bc_led_pulse(&led, 2000);

    // Initialize button
    //bc_button_init(&button, BC_GPIO_BUTTON, BC_GPIO_PULL_DOWN, false);
    //bc_button_set_event_handler(&button, button_event_handler, NULL);

    // Infra Grid Module
    bc_module_infra_grid_init(&infra);
    bc_module_infra_grid_set_event_handler(&infra, infragrid_event_handler, NULL);
    bc_module_infra_grid_set_update_interval(&infra, INFRAGRID_UPDATE_INTERVAL);

    // Initialize battery
    bc_module_battery_init();
    bc_module_battery_set_event_handler(battery_event_handler, NULL);
    bc_module_battery_set_update_interval(BATTERY_UPDATE_INTERVAL);

    // Radio init
    bc_radio_init(BC_RADIO_MODE_NODE_SLEEPING);
    bc_radio_pairing_request("printer-desk-monitor", VERSION);
}
