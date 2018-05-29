/**
 * @file    mod_pulsgen.c
 *
 * @brief   pulses generator module
 *
 * This module implements an API
 * to make real-time pulses generation using GPIO
 */

#include "mod_timer.h"
#include "mod_gpio.h"
#include "mod_pulsgen.h"




// private vars

static uint8_t max_id = 0; // maximum channel id
static struct pulsgen_ch_t gen[PULSGEN_CH_CNT] = {0}; // array of channels data




// public methods

/**
 * @brief   module init
 * @note    call this function only once before pulsgen_module_base_thread()
 * @retval  none
 */
void pulsgen_module_init()
{
    TIMER_START();
}

/**
 * @brief   module base thread
 * @note    call this function in the main loop, before gpio_module_base_thread()
 * @retval  none
 */
void pulsgen_module_base_thread()
{
    static uint8_t c;
    static uint32_t tick, todo_tick;

    // get current CPU tick
    tick = TIMER_CNT_GET();

    // check all working channels
    for ( c = max_id + 1; c--; )
    {
        if ( !gen[c].task ) continue; // if channel disabled, goto next channel

        if ( !gen[c].task_infinite && !gen[c].task_toggles_todo ) // if we have no steps to do
        {
            gen[c].task = 0; // disable channel
            if ( max_id && c == max_id ) --max_id; // if needed decrease channels max ID value
            continue; // goto next channel
        }

        if // if it's time to make a pulse change
        (
            ( !gen[c].todo_tick_ovrfl &&             tick  >= gen[c].todo_tick ) ||
            (  gen[c].todo_tick_ovrfl && (UINT32_MAX-tick) >= gen[c].todo_tick )
        )
        {
            todo_tick = gen[c].todo_tick; // save current tick value

            if ( gen[c].pin_state ) // if current pin state is HIGH
            {
                gen[c].pin_state = 0; // set pin state to LOW
                gen[c].todo_tick += gen[c].low_ticks; // set new timestamp
            }
            else // if current pin state is LOW
            {
                gen[c].pin_state = 1; // set step state to HIGH
                gen[c].todo_tick += gen[c].high_ticks; // set new timestamp
            }

            // set timestamp overflow flag
            gen[c].todo_tick_ovrfl = gen[c].todo_tick < todo_tick ? 1 : 0;

            --gen[c].task_toggles_todo; // decrease number of pin changes to do

            // toggle pin
            if ( gen[c].pin_state ^ gen[c].pin_inverted )
            {
                gpio_pin_clear(gen[c].port, gen[c].pin);
            }
            else
            {
                gpio_pin_set(gen[c].port, gen[c].pin);
            }
        }
    }
}




/**
 * @brief   setup GPIO pin for the selected channel
 *
 * @param   c           channel id
 * @param   port        GPIO port number
 * @param   pin         GPIO pin number
 * @param   inverted    invert pin state?
 *
 * @retval  none
 */
void pulsgen_pin_setup(uint8_t c, uint8_t port, uint8_t pin, uint8_t inverted)
{
    gpio_pin_setup_for_output(port, pin);

    gen[c].port = port;
    gen[c].pin = pin;
    gen[c].pin_inverted = inverted;
    gen[c].pin_state = inverted ? 1 : 0;

    // set pin state
    if ( gen[c].pin_state ^ gen[c].pin_inverted )
    {
        gpio_pin_clear(gen[c].port, gen[c].pin);
    }
    else
    {
        gpio_pin_set(gen[c].port, gen[c].pin);
    }
}




/**
 * @brief   setup a new task for the selected channel
 *
 * @param   c           channel id
 * @param   frequency   pin state change frequency (in Hz)
 * @param   toggles     number of pin state changes
 * @param   duty        duty cycle value (0..PULSGEN_MAX_DUTY)
 * @param   infinite    is this task infinite?
 *
 * @retval  none
 */
void pulsgen_task_setup(uint8_t c, uint32_t frequency, uint32_t toggles, uint32_t duty, uint8_t infinite)
{
    if ( c > max_id ) ++max_id;

    gen[c].task = 1;
    gen[c].task_infinite = infinite;
    gen[c].task_toggles = infinite ? UINT32_MAX : toggles;
    gen[c].task_toggles_todo = gen[c].task_toggles;

    gen[c].low_ticks  = TIMER_FREQUENCY / frequency * (PULSGEN_MAX_DUTY - duty) / PULSGEN_MAX_DUTY;
    gen[c].high_ticks = TIMER_FREQUENCY / frequency *                     duty  / PULSGEN_MAX_DUTY;

    gen[c].todo_tick = TIMER_CNT_GET();
    gen[c].todo_tick_ovrfl = 0;
}

/**
 * @brief   abort current task for the selected channel
 * @param   c       channel id
 * @retval  none
 */
void pulsgen_task_abort(uint8_t c)
{
    gen[c].task = 0;

    if ( max_id && c == max_id ) --max_id;
}




/**
 * @brief   get current task state for the selected channel
 *
 * @param   c   channel id
 *
 * @retval  0   (channel have no task)
 * @retval  1   (channel have a task)
 */
uint8_t pulsgen_task_state(uint8_t c)
{
    return gen[c].task;
}

/**
 * @brief   get current pin state changes since task start
 * @param   c   channel id
 * @retval  0..0xFFFFFFFF
 */
uint32_t pulsgen_task_toggles(uint8_t c)
{
    return gen[c].task_toggles - gen[c].task_toggles_todo;
}




/**
    @example mod_pulsgen.c

    <b>Usage example 1</b>: enable infinite PWM signal on GPIO pin PA3

    @code
        #include <stdint.h>
        #include "mod_gpio.h"
        #include "mod_pulsgen.h"

        int main(void)
        {
            // module init
            pulsgen_module_init();

            // use GPIO pin PA3 for the channel 0 output
            pulsgen_pin_setup(0, PA, 3, 0);

            // enable infinite PWM signal on the channel 0
            // PWM frequency = 25 kHz, duty cycle = 50%
            pulsgen_task_setup(0, 25000, 0, 50, 1);

            // main loop
            for(;;)
            {
                // real update of channel states
                pulsgen_module_base_thread();
                // real update of pin states
                gpio_module_base_thread();
            }

            return 0;
        }
    @endcode

    <b>Usage example 2</b>: output of STEP/DIR signal

    @code
        #include <stdint.h>
        #include "mod_gpio.h"
        #include "mod_pulsgen.h"

        #define STEP_CHANNEL 0
        #define DIR_CHANNEL 1

        int main(void)
        {
            // uses to switch between DIR an STEP output
            uint8_t dir_output = 0; // 0 = STEP output, 1 = DIR output

            // module init
            pulsgen_module_init();

            // use GPIO pin PA3 for the STEP output on the channel 0
            pulsgen_pin_setup(STEP_CHANNEL, PA, 3, 0);

            // use GPIO pin PA5 for the DIR output on the channel 1
            pulsgen_pin_setup(DIR_CHANNEL, PA, 5, 0);

            // main loop
            for(;;)
            {
                if // if both channels aren't busy
                (
                    ! pulsgen_task_state(STEP_CHANNEL) &&
                    ! pulsgen_task_state(DIR_CHANNEL)
                )
                {
                    if ( dir_output ) // if it's time to make a DIR change
                    {
                        // make a DIR change with 1 kHz rate and 50% duty cycle
                        pulsgen_task_setup(DIR_CHANNEL, 1000, 1, 50, 0);
                        dir_output = 0;
                    }
                    else // if it's time to make a STEP output
                    {
                        // start output of 1000 steps with 25 kHz rate and 50% duty cycle
                        pulsgen_task_setup(STEP_CHANNEL, 25000, 2000, 50, 0);
                        dir_output = 1;
                    }
                }

                // real update of channel states
                pulsgen_module_base_thread();
                // real update of pin states
                gpio_module_base_thread();
            }

            return 0;
        }
    @endcode
*/