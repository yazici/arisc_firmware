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
static uint8_t msg_buf[PULSGEN_MSG_BUF_LEN] = {0};
static uint64_t tick = 0, wd_ticks = 0, wd_todo_tick = 0;
static struct pulsgen_fifo_item_t fifo[PULSGEN_CH_CNT][PULSGEN_FIFO_SIZE] = {{0}};
static uint8_t fifo_pos[PULSGEN_CH_CNT] = {0};

// uses with GPIO module macros
extern volatile uint32_t * gpio_port_data[GPIO_PORTS_CNT];




// private function prototypes

static void abort(uint8_t c);
static void task_setup
(
    uint32_t c,
    uint32_t toggles_dir,
    uint32_t toggles,
    uint32_t pin_setup_time,
    uint32_t pin_hold_time,
    uint32_t start_delay
);





// public methods

/**
 * @brief   module init
 * @note    call this function only once before pulsgen_module_base_thread()
 * @retval  none
 */
void pulsgen_module_init()
{
    uint8_t i = 0;

    // start sys timer
    TIMER_START();

    // add message handlers
    for ( i = PULSGEN_MSG_PIN_SETUP; i < PULSGEN_MSG_CNT; i++ )
    {
        msg_recv_callback_add(i, (msg_recv_func_t) pulsgen_msg_recv);
    }
}

/**
 * @brief   module base thread
 * @note    call this function in the main loop, before gpio_module_base_thread()
 * @retval  none
 */
void pulsgen_module_base_thread()
{
    static uint8_t c, abort_all = 0;

    // get current CPU tick
    tick = timer_cnt_get_64();

    // have we a watchdog? && watchdog time is over?
    if ( wd_todo_tick && tick > wd_todo_tick ) abort_all = 1; // set abort flag

    // check all working channels
    for ( c = max_id + 1; c--; )
    {
        // channel disabled?
        if ( !gen[c].task ) continue;
        // watchdog time is over?
        if ( abort_all ) { abort(c); continue; }
        // it's not a time for a pulse?
        if ( tick < gen[c].todo_tick ) continue;
        // no steps to do?
        if ( !gen[c].task_toggles_todo && !gen[c].task_infinite )
        {
            // goto new fifo item
            fifo[c][fifo_pos[c]].used = 0;
            if ( (++fifo_pos[c]) >= PULSGEN_FIFO_SIZE ) fifo_pos[c] = 0;

            // have we a new task in the fifo?
            if ( fifo[c][fifo_pos[c]].used ) // setup new task
            {
                task_setup(c,
                    fifo[c][fifo_pos[c]].toggles_dir,
                    fifo[c][fifo_pos[c]].toggles,
                    fifo[c][fifo_pos[c]].pin_setup_time,
                    fifo[c][fifo_pos[c]].pin_hold_time,
                    fifo[c][fifo_pos[c]].start_delay);
            }
            else // disable channel
            {
                gen[c].task = 0;
                if ( max_id && c == max_id ) --max_id;
            }

            continue;
        }

        // pin state is HIGH?
        if ( GPIO_PIN_GET(gen[c].port, gen[c].pin_mask) ^ gen[c].pin_inverted )
        {
            GPIO_PIN_CLEAR(gen[c].port, gen[c].pin_mask_not);
            if ( gen[c].abort_on_setup ) abort(c);
            else gen[c].todo_tick += (uint64_t)gen[c].setup_ticks;
        }
        else // pin state is LOW
        {
            GPIO_PIN_SET(gen[c].port, gen[c].pin_mask);
            if ( gen[c].abort_on_hold ) abort(c);
            else gen[c].todo_tick += (uint64_t)gen[c].hold_ticks;
        }

        // decrease pin toggles to do
        --gen[c].task_toggles_todo;

        // update total toggles value
        gen[c].cnt += gen[c].task_toggles * (gen[c].toggles_dir ? -1 : 1);
    }

    // watchdog time is over?
    if ( abort_all ) abort_all = 0; // reset abort flag
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
    gen[c].pin_mask = 1U << pin;
    gen[c].pin_mask_not = ~(gen[c].pin_mask);
    gen[c].pin_inverted = inverted ? gen[c].pin_mask : 0;

    // set pin state
    if ( gen[c].pin_inverted )  GPIO_PIN_SET    (port, gen[c].pin_mask);
    else                        GPIO_PIN_CLEAR  (port, gen[c].pin_mask_not);
}




/**
 * @brief   add a new task for the selected channel
 *
 * @param   c               channel id
 * @param   toggles         number of pin state changes
 * @param   toggles_dir     0 = cnt++, !0 = cnt--
 * @param   pin_setup_time  pin state setup_time (in nanoseconds)
 * @param   pin_hold_time   pin state hold_time (in nanoseconds)
 * @param   start_delay     task start delay (in nanoseconds)
 *
 * @retval  none
 */
void pulsgen_task_add
(
    uint32_t c,
    uint32_t toggles_dir,
    uint32_t toggles,
    uint32_t pin_setup_time,
    uint32_t pin_hold_time,
    uint32_t start_delay
)
{
    uint8_t i, pos;

    // channel is busy?
    if ( gen[c].task )
    {
        // find free fifo slot for the new task
        for ( i = PULSGEN_FIFO_SIZE, pos = fifo_pos[c] + 1; i--; pos++ )
        {
            if ( pos >= PULSGEN_FIFO_SIZE ) pos = 0;
            if ( fifo[c][pos].used ) continue;

            fifo[c][pos].used = 1;
            fifo[c][pos].toggles_dir = toggles_dir;
            fifo[c][pos].toggles = toggles;
            fifo[c][pos].pin_setup_time = pin_setup_time;
            fifo[c][pos].pin_hold_time = pin_hold_time;
            fifo[c][pos].start_delay = start_delay;

            return;
        }

        return;
    }

    // block current fifo item
    fifo[c][fifo_pos[c]].used = 1;

    // setup current task
    task_setup(c, toggles_dir, toggles, pin_setup_time, pin_hold_time, start_delay);
}

static void task_setup
(
    uint32_t c,
    uint32_t toggles_dir,
    uint32_t toggles,
    uint32_t pin_setup_time,
    uint32_t pin_hold_time,
    uint32_t start_delay
)
{
    if ( c > max_id ) ++max_id;

    // set task data
    gen[c].task = 1;
    gen[c].task_infinite = toggles ? 0 : 1;
    gen[c].toggles_dir = toggles_dir;
    gen[c].task_toggles = toggles ? toggles : UINT32_MAX;
    gen[c].task_toggles_todo = gen[c].task_toggles;
    gen[c].abort_on_hold = 0;
    gen[c].abort_on_setup = 0;

    gen[c].setup_ticks = (uint32_t) ( (uint64_t)pin_setup_time *
        (uint64_t)TIMER_FREQUENCY_MHZ / (uint64_t)1000 );
    gen[c].hold_ticks = (uint32_t) ( (uint64_t)pin_hold_time *
        (uint64_t)TIMER_FREQUENCY_MHZ / (uint64_t)1000 );

    gen[c].todo_tick = tick;

    // if we need a delay before task start
    if ( start_delay )
    {
        gen[c].todo_tick += (uint64_t)start_delay *
            (uint64_t)TIMER_FREQUENCY_MHZ / (uint64_t)1000;
    }
}



/**
 * @brief   abort current task for the selected channel
 * @param   c           channel id
 * @param   on_hold     0 = on pin state setup, !0 = on hold
 * @retval  none
 */
void pulsgen_abort(uint8_t c, uint8_t on_hold)
{
    // pin state is HIGH?
    if ( GPIO_PIN_GET(gen[c].port, gen[c].pin_mask) ^ gen[c].pin_inverted )
    {
        // abort on pin hold?
        if ( on_hold ) { abort(c); return; }
    }
    else // pin state is LOW
    {
        // abort on pin setup?
        if ( !on_hold ) { abort(c); return; }
    }

    // abort on pin hold?
    if ( on_hold ) gen[c].abort_on_hold = 1;
    // abort on pin setup
    else gen[c].abort_on_setup = 1;
}

static void abort(uint8_t c)
{
    uint8_t i;

    gen[c].abort_on_hold = 0;
    gen[c].abort_on_setup = 0;
    gen[c].task = 0;

    if ( max_id && c == max_id ) --max_id;

    // fifo cleanup
    for ( i = PULSGEN_FIFO_SIZE; i--; ) fifo[c][i].used = 0;
}




/**
 * @brief   get current task state for the selected channel
 *
 * @param   c   channel id
 *
 * @retval  0   (channel have no task)
 * @retval  1   (channel have a task)
 */
uint8_t pulsgen_state_get(uint8_t c)
{
    return gen[c].task;
}




/**
 * @brief   get current pin state changes since task start
 * @param   c   channel id
 * @retval  0..0xFFFFFFFF
 */
uint32_t pulsgen_task_toggles_get(uint8_t c)
{
    return gen[c].task_toggles - gen[c].task_toggles_todo;
}




/**
 * @brief   get total pin toggles
 * @param   c   channel id
 * @retval  integer 4-bytes
 */
int32_t pulsgen_cnt_get(uint8_t c)
{
    return gen[c].cnt;
}

/**
 * @brief   set total pin toggles value
 * @param   c       channel id
 * @param   value   integer 4-bytes
 * @retval  none
 */
void pulsgen_cnt_set(uint8_t c, int32_t value)
{
    gen[c].cnt = value;
}




/**
 * @brief   get total pin toggles
 * @param   c   channel id
 * @retval  0..0xFFFFFFFF
 */
uint32_t pulsgen_tasks_done_get(uint8_t c)
{
    return gen[c].tasks_done;
}

/**
 * @brief   set total pin toggles value
 * @param   c       channel id
 * @param   tasks   tasks count (0..0xFFFFFFFF)
 * @retval  none
 */
void pulsgen_tasks_done_set(uint8_t c, uint32_t tasks)
{
    gen[c].tasks_done = tasks;
}




/**
 * @brief   enable/disable `abort all` watchdog
 * @param   enable      0 = disable watchdog, other values - enable watchdog
 * @param   time        watchdog wait time (in nanoseconds)
 * @retval  none
 */
void pulsgen_watchdog_setup(uint8_t enable, uint32_t time)
{
    if ( !enable ) { wd_todo_tick = 0; return; }

    wd_ticks = (uint64_t)time * (uint64_t)TIMER_FREQUENCY_MHZ / (uint64_t)1000;
    wd_todo_tick = tick + wd_ticks;
}




/**
 * @brief   "message received" callback
 *
 * @note    this function will be called automatically
 *          when a new message will arrive for this module.
 *
 * @param   type    user defined message type (0..0xFF)
 * @param   msg     pointer to the message buffer
 * @param   length  the length of a message (0 .. MSG_LEN)
 *
 * @retval   0 (message read)
 * @retval  -1 (message not read)
 */
int8_t volatile pulsgen_msg_recv(uint8_t type, uint8_t * msg, uint8_t length)
{
    static uint8_t i = 0;

    // any incoming message will update the watchdog wait time
    if ( wd_todo_tick ) wd_todo_tick = tick + wd_ticks;

    u32_10_t in = *((u32_10_t*) msg);
    u32_10_t out = *((u32_10_t*) &msg_buf);

    switch (type)
    {
        case PULSGEN_MSG_PIN_SETUP:
            pulsgen_pin_setup(in.v[0], in.v[1], in.v[2], in.v[3]);
            break;
        case PULSGEN_MSG_TASK_ADD:
            pulsgen_task_add(in.v[0], in.v[1], in.v[2], in.v[3], in.v[4], in.v[5]);
            break;
        case PULSGEN_MSG_ABORT:
            pulsgen_abort(in.v[0], in.v[1]);
            break;
        case PULSGEN_MSG_STATE_GET:
            out.v[0] = pulsgen_state_get(in.v[0]);
            msg_send(type, msg_buf, 4);
            break;
        case PULSGEN_MSG_TASK_TOGGLES_GET:
            out.v[0] = pulsgen_task_toggles_get(in.v[0]);
            msg_send(type, msg_buf, 4);
            break;
        case PULSGEN_MSG_CNT_GET:
            out.v[0] = pulsgen_cnt_get(in.v[0]);
            msg_send(type, msg_buf, 4);
            break;
        case PULSGEN_MSG_CNT_SET:
            pulsgen_cnt_set(in.v[0], (int32_t)in.v[1]);
            break;
        case PULSGEN_MSG_TASKS_DONE_GET:
            out.v[0] = pulsgen_tasks_done_get(in.v[0]);
            msg_send(type, msg_buf, 4);
            break;
        case PULSGEN_MSG_TASKS_DONE_SET:
            pulsgen_tasks_done_set(in.v[0], in.v[1]);
            break;
        case PULSGEN_MSG_WATCHDOG_SETUP:
            pulsgen_watchdog_setup(in.v[0], in.v[1]);
            break;

        default: return -1;
    }

    return 0;
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
            // PWM frequency = 20 kHz, duty cycle = 50%
            pulsgen_task_add(0, 0, 0, 25000, 25000, 0);

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
                    ! pulsgen_state_get(STEP_CHANNEL) &&
                    ! pulsgen_state_get(DIR_CHANNEL)
                )
                {
                    if ( dir_output ) // if it's time to make a DIR change
                    {
                        // make a DIR change with 20 kHz rate and 50% duty cycle
                        pulsgen_task_add(DIR_CHANNEL, 0, 1, 25000, 25000, 0);
                        dir_output = 0;
                    }
                    else // if it's time to make a STEP output
                    {
                        // start output of 1000 steps with 20 kHz rate,
                        // 50% duty cycle and startup delay = 50 us
                        pulsgen_task_add(STEP_CHANNEL, 0, 2000, 25000, 25000, 50000);
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
