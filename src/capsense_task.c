/*******************************************************************************
* File Name: capsense_task.c
*
* Description: This file contains the task that handles touch sensing.
*
* Related Document: README.md
*
*******************************************************************************
* Copyright 2019-2023, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/


/******************************************************************************
* Header files includes
******************************************************************************/
#include "cybsp.h"
#include "cycfg.h"
#include "cyhal.h"
#include "cycfg_capsense.h"
#include "capsense_task.h"
#include "led_task.h"


/*******************************************************************************
* Global constants
*******************************************************************************/
#define CAPSENSE_INTERRUPT_PRIORITY    (7u)
#define EZI2C_INTERRUPT_PRIORITY    (6u)    /* EZI2C interrupt priority must be
                                             * higher than CAPSENSE interrupt
                                             */
#define CAPSENSE_SCAN_INTERVAL_MS    (15u)   /* in milliseconds*/


/*******************************************************************************
* Function Prototypes
*******************************************************************************/
static uint32_t capsense_init(void);
static void tuner_init(void);
static void process_touch(void);
static void capsense_isr(void);
static void capsense_end_of_scan_callback(cy_stc_active_scan_sns_t* active_scan_sns_ptr);
static void capsense_timer_callback(struct k_timer *params);
void handle_error(void);


/******************************************************************************
* Global variables
******************************************************************************/
cy_stc_scb_ezi2c_context_t ezi2c_context;
cyhal_ezi2c_t sEzI2C;
cyhal_ezi2c_slave_cfg_t sEzI2C_sub_cfg;
cyhal_ezi2c_cfg_t sEzI2C_cfg;

/* Queue handle used for LED data */
K_FIFO_DEFINE(capsense_command_q);
/* Initialize timer for periodic CAPSENSE scan */
K_TIMER_DEFINE(scan_timer_handle, capsense_timer_callback, NULL);

/* SysPm callback params */
cy_stc_syspm_callback_params_t callback_params =
{
    .base       = CYBSP_CSD_HW,
    .context    = &cy_capsense_context
};

cy_stc_syspm_callback_t capsense_deep_sleep_cb =
{
    Cy_CapSense_DeepSleepCallback,
    CY_SYSPM_DEEPSLEEP,
    (CY_SYSPM_SKIP_CHECK_FAIL | CY_SYSPM_SKIP_BEFORE_TRANSITION | CY_SYSPM_SKIP_AFTER_TRANSITION),
    &callback_params,
    NULL,
    NULL
};


/*******************************************************************************
* Function Name: handle_error
********************************************************************************
* Summary:
* User defined error handling function
*
* Parameters:
*  void
*
* Return:
*  void
*
*******************************************************************************/
void handle_error(void)
{
    /* Disable all interrupts. */
    __disable_irq();

    CY_ASSERT(0);
}


/*******************************************************************************
* Function Name: task_capsense
********************************************************************************
* Summary:
*  Task that initializes the CAPSENSE block and processes the touch input.
*
* Parameters:
*  void *param : Task parameter defined during task creation (unused)
*
*******************************************************************************/
void task_capsense(void *dummy1, void *dummy2, void *dummy3)
{
    cy_status status;
    capsense_command_t *capsense_cmd;

    /* Remove warning for unused parameter */
    ARG_UNUSED(dummy1);
	ARG_UNUSED(dummy2);
	ARG_UNUSED(dummy3);

    /* Setup communication between Tuner GUI and PSoC 6 MCU */
    tuner_init();

    /* Initialize CAPSENSE block */
    status = capsense_init();
    if(CY_RET_SUCCESS != status)
    {
        CY_ASSERT(0u);
    }

    /* Start the timer */
    k_timer_start(&scan_timer_handle, K_NO_WAIT,
                    K_MSEC(CAPSENSE_SCAN_INTERVAL_MS));

    /* Repeatedly running part of the task */
    for(;;)
    {
        /* Block until a CAPSENSE command has been received over queue */
        capsense_cmd = k_fifo_get(&capsense_command_q, K_FOREVER);

        /* Command has been received from capsense_cmd */
        /* Check if CAPSENSE is busy with a previous scan */
        if(CY_CAPSENSE_NOT_BUSY == Cy_CapSense_IsBusy(&cy_capsense_context))
        {
            switch(*capsense_cmd)
            {
                case CAPSENSE_SCAN:
                {
                    /* Start scan */
                    Cy_CapSense_ScanAllWidgets(&cy_capsense_context);
                    break;
                }
                case CAPSENSE_PROCESS:
                {
                    /* Process all widgets */
                    Cy_CapSense_ProcessAllWidgets(&cy_capsense_context);
                    process_touch();

                    /* Establishes synchronized operation between the CAPSENSE
                        * middleware and the CAPSENSE Tuner tool.
                        */
                    Cy_CapSense_RunTuner(&cy_capsense_context);
                    break;
                }
                /* Invalid command */
                default:
                {
                    break;
                }
            }
        }
    }
}


/*******************************************************************************
* Function Name: process_touch
********************************************************************************
* Summary:
*  This function processes the touch input and sends command to LED task.
*
*******************************************************************************/
static void process_touch(void)
{
    /* Variables used to store touch information */
    uint32_t button0_status = 0;
    uint32_t button1_status = 0;
    uint16_t slider_pos = 0;
    uint8_t slider_touched = 0;
    cy_stc_capsense_touch_t *slider_touch;

    /* Variables used to store previous touch information */
    static uint32_t button0_status_prev = 0;
    static uint32_t button1_status_prev = 0;
    static uint16_t slider_pos_perv = 0;

    /* Variables used for storing command and data for LED Task */
    led_command_data_t led_cmd_data;
    bool send_led_command = false;

    /* Get button 0 status */
    button0_status = Cy_CapSense_IsSensorActive(
        CY_CAPSENSE_BUTTON0_WDGT_ID,
        CY_CAPSENSE_BUTTON0_SNS0_ID,
        &cy_capsense_context);

    /* Get button 1 status */
    button1_status = Cy_CapSense_IsSensorActive(
        CY_CAPSENSE_BUTTON1_WDGT_ID,
        CY_CAPSENSE_BUTTON1_SNS0_ID,
        &cy_capsense_context);

    /* Get slider status */
    slider_touch = Cy_CapSense_GetTouchInfo(
        CY_CAPSENSE_LINEARSLIDER0_WDGT_ID,
        &cy_capsense_context);
    slider_pos = slider_touch->ptrPosition->x;
    slider_touched = slider_touch->numPosition;

    /* Detect new touch on Button0 */
    if((0u != button0_status) && (0u == button0_status_prev))
    {
        led_cmd_data.command = LED_TURN_ON;
        send_led_command = true;
    }

    /* Detect new touch on Button1 */
    if((0u != button1_status) && (0u == button1_status_prev))
    {
        led_cmd_data.command = LED_TURN_OFF;
        send_led_command = true;
    }

    /* Detect new touch on slider */
    if((0u != slider_touched) && (slider_pos_perv != slider_pos ))
    {
        led_cmd_data.command = LED_UPDATE_BRIGHTNESS;

        /* brightness in percentage */
        led_cmd_data.brightness = (slider_pos * 100)
                    / cy_capsense_context.ptrWdConfig[CY_CAPSENSE_LINEARSLIDER0_WDGT_ID].xResolution;
        send_led_command = true;
    }

    /* Send command to update LED state if required */
    if(send_led_command)
    {
        k_fifo_put(&led_command_data_q, &led_cmd_data);
    }

    /* Update previous touch status */
    button0_status_prev = button0_status;
    button1_status_prev = button1_status;
    slider_pos_perv = slider_pos;
}


/*******************************************************************************
* Function Name: capsense_init
********************************************************************************
* Summary:
*  This function initializes the CSD HW block, and configures the CAPSENSE
*  interrupt.
*
*******************************************************************************/
static uint32_t capsense_init(void)
{
    uint32_t status = CYRET_SUCCESS;

    /* CAPSENSE interrupt configuration parameters */
    static const cy_stc_sysint_t capSense_intr_config =
    {
        .intrSrc = csd_interrupt_IRQn,
        .intrPriority = CAPSENSE_INTERRUPT_PRIORITY,
    };

    /*Initialize CAPSENSE Data structures */
    status = Cy_CapSense_Init(&cy_capsense_context);
    if (CYRET_SUCCESS != status)
    {
        return status;
    }

    /* Initialize CAPSENSE interrupt */
    cyhal_system_set_isr(csd_interrupt_IRQn, csd_interrupt_IRQn, CAPSENSE_INTERRUPT_PRIORITY, &capsense_isr);
    NVIC_ClearPendingIRQ(capSense_intr_config.intrSrc);
    NVIC_EnableIRQ(capSense_intr_config.intrSrc);

    /* Initialize the CAPSENSE deep sleep callback functions. */
    Cy_CapSense_Enable(&cy_capsense_context);
    if (CYRET_SUCCESS != status)
    {
        return status;
    }
    Cy_SysPm_RegisterCallback(&capsense_deep_sleep_cb);
    /* Register end of scan callback */
    status = Cy_CapSense_RegisterCallback(CY_CAPSENSE_END_OF_SCAN_E,
                                              capsense_end_of_scan_callback, &cy_capsense_context);
    if (CYRET_SUCCESS != status)
    {
        return status;
    }

    return status;
}


/*******************************************************************************
* Function Name: capsense_end_of_scan_callback
********************************************************************************
* Summary:
*  CAPSENSE end of scan callback function. This function sends a command to
*  CAPSENSE task to process scan.
*
* Parameters:
*  cy_stc_active_scan_sns_t * active_scan_sns_ptr (unused)
*
*******************************************************************************/
static void capsense_end_of_scan_callback(cy_stc_active_scan_sns_t* active_scan_sns_ptr)
{
    ARG_UNUSED(active_scan_sns_ptr);

    /* Send command to process CAPSENSE data */
    static capsense_command_t command = CAPSENSE_PROCESS;
    k_fifo_put(&capsense_command_q, &command);
}


/*******************************************************************************
* Function Name: capsense_timer_callback
********************************************************************************
* Summary:
*  CAPSENSE timer callback. This function sends a command to start CAPSENSE
*  scan.
*
* Parameters:
*  struct k_timer dummy (unused)
*
*******************************************************************************/
static void capsense_timer_callback(struct k_timer *dummy)
{
    Cy_CapSense_Wakeup(&cy_capsense_context);
    static capsense_command_t command = CAPSENSE_SCAN;

    ARG_UNUSED(dummy);

    /* Send command to start CAPSENSE scan */
    k_fifo_put(&capsense_command_q, &command);
}


/*******************************************************************************
* Function Name: capsense_isr
********************************************************************************
* Summary:
*  Wrapper function for handling interrupts from CSD block.
*
*******************************************************************************/
static void capsense_isr(void)
{
    Cy_CapSense_InterruptHandler(CYBSP_CSD_HW, &cy_capsense_context);
}


/*******************************************************************************
* Function Name: tuner_init
********************************************************************************
* Summary:
*  Initializes communication between Tuner GUI and PSoC 6 MCU.
*
*******************************************************************************/
static void tuner_init(void)
{
    cy_rslt_t result;
    /* Configure CAPSENSE Tuner as EzI2C Slave */
    sEzI2C_sub_cfg.buf = (uint8_t *)&cy_capsense_tuner;
    sEzI2C_sub_cfg.buf_rw_boundary = sizeof(cy_capsense_tuner);
    sEzI2C_sub_cfg.buf_size = sizeof(cy_capsense_tuner);
    sEzI2C_sub_cfg.slave_address = 8U;

    sEzI2C_cfg.data_rate = CYHAL_EZI2C_DATA_RATE_400KHZ;
    sEzI2C_cfg.enable_wake_from_sleep = true;
    sEzI2C_cfg.slave1_cfg = sEzI2C_sub_cfg;
    sEzI2C_cfg.sub_address_size = CYHAL_EZI2C_SUB_ADDR16_BITS;
    sEzI2C_cfg.two_addresses = false;
    result = cyhal_ezi2c_init( &sEzI2C, CYBSP_I2C_SDA, CYBSP_I2C_SCL, NULL, &sEzI2C_cfg);
    if (result != CY_RSLT_SUCCESS)
    {
        handle_error();
    }

}


/* END OF FILE [] */
