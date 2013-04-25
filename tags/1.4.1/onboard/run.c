/*
    This file is part of AutoQuad ESC32.

    AutoQuad ESC32 is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    AutoQuad ESC32 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with AutoQuad ESC32.  If not, see <http://www.gnu.org/licenses/>.

    Copyright © 2011, 2012  Bill Nesbitt
*/

#include "run.h"
#include "main.h"
#include "timer.h"
#include "adc.h"
#include "fet.h"
#include "pwm.h"
#include "cli.h"
#include "binary.h"
#include "config.h"
#include "misc.h"
#include "stm32f10x_exti.h"
#include "stm32f10x_pwr.h"
#include "stm32f10x_iwdg.h"
#include "stm32f10x_dbgmcu.h"
#include <math.h>

uint32_t runMilis;
uint32_t oldIdleCounter;
float idlePercent;
float avgAmps, maxAmps;
float avgVolts;
float rpm;
float targetRpm;
float rpmI;
float runRPMFactor;
float maxCurrentSQRT;
uint8_t disarmReason;
uint8_t commandMode;
uint8_t runArmCount;
volatile uint8_t runMode;

void runFeedIWDG(void) {
#ifdef RUN_ENABLE_IWDG
    IWDG_ReloadCounter();
#endif
}

// setup the hardware independent watchdog
uint16_t runIWDGInit(int ms) {
    uint16_t prevReloadVal;
    int reloadVal;

#ifndef RUN_ENABLE_IWDG
    return 0;
#endif
    IWDG_ReloadCounter();

    DBGMCU_Config(DBGMCU_IWDG_STOP, ENABLE);

    // IWDG timeout equal to 10 ms (the timeout may varies due to LSI frequency dispersion)
    // Enable write access to IWDG_PR and IWDG_RLR registers
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);

    // IWDG counter clock: LSI/4
    IWDG_SetPrescaler(IWDG_Prescaler_4);

    // Set counter reload value to obtain 10ms IWDG TimeOut.
    //  Counter Reload Value	= 10ms/IWDG counter clock period
    //				= 10ms / (RUN_LSI_FREQ/4)
    //				= 0.01s / (RUN_LSI_FREQ/4)
    //				= RUN_LSI_FREQ/(4 * 100)
    //				= RUN_LSI_FREQ/400
    reloadVal = RUN_LSI_FREQ*ms/4000;

    if (reloadVal < 1)
	reloadVal = 1;
    else if (reloadVal > 0xfff)
	reloadVal = 0xfff;

    prevReloadVal = IWDG->RLR;

    IWDG_SetReload(reloadVal);

    // Reload IWDG counter
    IWDG_ReloadCounter();

    // Enable IWDG (the LSI oscillator will be enabled by hardware)
    IWDG_Enable();

    return (prevReloadVal*4000/RUN_LSI_FREQ);
}

void runDisarm(int reason) {
    fetSetDutyCycle(0);
    timerCancelAlarm2();
    state = ESC_STATE_DISARMED;
    pwmIsrAllOn();
    digitalHi(statusLed);   // turn off
    digitalLo(errorLed);    // turn on
    disarmReason = reason;
}

void runArm(void) {
    fetSetDutyCycle(0);
    timerCancelAlarm2();
    digitalHi(errorLed);
    state = ESC_STATE_STOPPED;
    digitalLo(statusLed);   // turn on
    if (inputMode == ESC_INPUT_UART)
	runMode = OPEN_LOOP;
    fetSetBraking(0);
    fetBeep(150, 800);
}

void runStart(void) {
    state = ESC_STATE_STARTING;
    fetStartCommutation();
}

void runStop(void) {
    runMode = OPEN_LOOP;
    fetSetDutyCycle(0);
}

uint8_t runDuty(float duty) {
    uint8_t ret = 0;

    if (duty >= 0.0f || duty <= 100.0f) {
	runMode = OPEN_LOOP;
	fetSetBraking(0);
	fetSetDutyCycle((uint16_t)(fetPeriod*duty*0.01f));
	ret = 1;
    }

    return ret;
}

void runNewInput(uint16_t setpoint) {
    static uint16_t lastPwm;

    if (state == ESC_STATE_RUNNING && setpoint != lastPwm) {
	if (runMode == OPEN_LOOP) {
	    fetSetDutyCycle(fetPeriod * (int32_t)(setpoint-pwmLoValue) / (int32_t)(pwmHiValue - pwmLoValue));
	}
	else if (runMode == CLOSED_LOOP_RPM) {
	    float target = p[PWM_RPM_SCALE] * (setpoint-pwmLoValue) / (pwmHiValue - pwmLoValue);

	    // limit to configured maximum
	    targetRpm = (target > p[PWM_RPM_SCALE]) ? p[PWM_RPM_SCALE] : target;
	}
	lastPwm = setpoint;
    }
    else if (state == ESC_STATE_STARTING && setpoint <= pwmLoValue) {
	fetSetDutyCycle(0);
	state = ESC_STATE_RUNNING;
    }
    else if (state == ESC_STATE_DISARMED && setpoint > pwmMinValue && setpoint <= pwmLoValue) {
	runArmCount++;
	if (runArmCount > RUN_ARM_COUNT)
	    runArm();
    }
    else {
	runArmCount = 0;
    }

    if (state == ESC_STATE_STOPPED && setpoint >= pwmMinStart) {
	runStart();
    }
}

void runWatchDog(void) {
    register uint32_t t, d, p;

    __asm volatile ("cpsid i");
    t = timerMicros;
    d = detectedCrossing;
    p = pwmValidMicros;
    __asm volatile ("cpsie i");

    if (state == ESC_STATE_STARTING && fetGoodDetects > fetStartDetects) {
	state = ESC_STATE_RUNNING;
	digitalHi(statusLed);   // turn off
    }
    else if (state >= ESC_STATE_STOPPED) {   // running or starting
	d = (t >= d) ? (t - d) : (TIMER_MASK - d + t);

	// timeout if PWM signal disappears
	if (inputMode == ESC_INPUT_PWM) {
	    p = (t >= p) ? (t - p) : (TIMER_MASK - p + t);

	    if (p > PWM_TIMEOUT)
		runDisarm(REASON_PWM_TIMEOUT);
	}

//	if (state == ESC_STATE_RUNNING && d > ADC_CROSSING_TIMEOUT) {
	if (state >= ESC_STATE_STARTING && d > ADC_CROSSING_TIMEOUT) {
	    if (fetDutyCycle > 0) {
		runDisarm(REASON_CROSSING_TIMEOUT);
	    }
	    else {
		runArm();
		pwmIsrRunOn();
	    }
	}
	else if (state >= ESC_STATE_STARTING && fetBadDetects > fetDisarmDetects) {
	    if (fetDutyCycle > 0)
		runDisarm(REASON_BAD_DETECTS);
	}
	else if (state == ESC_STATE_STOPPED) {
	    adcAmpsOffset = adcAvgAmps;	// record current amperage offset
	}
    }
    else if (state == ESC_STATE_DISARMED && !(runMilis % 100)) {
	adcAmpsOffset = adcAvgAmps;	// record current amperage offset
	digitalTogg(errorLed);
    }
}

void runRpmPIDReset(void) {
    rpmI = 0.0f;
}

int32_t runRpmPID(float rpm, float target) {
    float error;
    float ff, rpmP, rpmD;
    float iTerm = rpmI;
    float output;

    // feed forward
    ff = ((target*target* p[FF1TERM] + target*p[FF2TERM]) / avgVolts) * fetPeriod;

    error = (target - rpm);

    if (error > 1000.0f)
	error = 1000.0f;

    if (error > 0.0f) {
	rpmP = error * p[PTERM];
	rpmI += error * p[ITERM];
    }
    else {
	rpmP =  error * p[PTERM] * 10.0f;
	rpmI += error * p[ITERM] * 0.15f;
    }

    if (fetBrakingEnabled) {
	if (rpm < 300.0f) {
	    fetSetBraking(0);
	}
	else if (error <= -100.0f) {
	    fetSetBraking(1);
	}
	else if (fetBraking && error > -25.0f){
	    fetSetBraking(0);
	}
    }

    output = ff + (rpmP + rpmI) * (1.0f / 1500.0f) * fetPeriod;

    // don't allow integral to continue to rise if at max output
    if (output >= fetPeriod)
	rpmI = iTerm;

    return output;
}

uint8_t runRpm(void) {
    if (state > ESC_STATE_STARTING) {
//	rpm = rpm * 0.90f + (runRPMFactor / (float)crossingPeriod) * 0.10f;
//	rpm -= (rpm - (runRPMFactor / (float)crossingPeriod)) * 0.25f;
	rpm = (rpm + (runRPMFactor / (float)crossingPeriod)) * 0.5f;

	// run closed loop control
	if (runMode == CLOSED_LOOP_RPM) {
	    fetSetDutyCycle(runRpmPID(rpm, targetRpm));
	    return 1;
	}
	else {
	    return 0;
	}
    }
    else {
	rpm = 0.0f;
	return 0;
    }
}

void runSetupPVD(void) {
    EXTI_InitTypeDef EXTI_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    // Configure EXTI Line16(PVD Output) to generate an interrupt on rising and falling edges
    EXTI_ClearITPendingBit(EXTI_Line16);
    EXTI_InitStructure.EXTI_Line = EXTI_Line16;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);

    // Enable the PVD Interrupt
    NVIC_InitStructure.NVIC_IRQChannel = PVD_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    // Configure the PVD Level to 2.2V
    PWR_PVDLevelConfig(PWR_PVDLevel_2V2);

    // Enable the PVD Output
    PWR_PVDCmd(ENABLE);
}

void runInit(void) {
    runSetupPVD();
    runSetConstants();
    runMode = p[STARTUP_MODE];

    SysTick_Config(SystemCoreClock / 1000); // 1ms
    NVIC_SetPriority(SysTick_IRQn, 2);	    // lower priority

    // setup hardware watchdog
    runIWDGInit(20);
}

#define RUN_CURRENT_ITERM	1.0f
#define RUN_CURRENT_PTERM	10.0f
#define RUN_MAX_DUTY_INCREASE	1.0f

float currentIState;

int32_t runCurrentPID(int32_t duty) {
    float error;
    float pTerm, iTerm;
    float out;

    error = avgAmps - p[MAX_CURRENT];

    currentIState += error;
    if (currentIState < 0.0f)
	currentIState = 0.0f;
    iTerm = currentIState * RUN_CURRENT_ITERM;

    pTerm = error * RUN_CURRENT_PTERM;
    if (pTerm < 0.0f)
	pTerm = 0.0f;

    duty = duty - iTerm - pTerm;

    if (duty < 0)
	duty = 0;

    return duty;
}

void runThrotLim(int32_t duty) {
    float maxVolts;
    int32_t maxDuty;

    // only if a limit is set
    if (p[MAX_CURRENT] > 0.0f) {
	// if current limiter is calibrated - best performance
	if (p[CL1TERM] != 0.0f) {
	    maxVolts = p[CL1TERM] + p[CL2TERM]*rpm + p[CL3TERM]*p[MAX_CURRENT] + p[CL4TERM]*rpm*maxCurrentSQRT + p[CL5TERM]*maxCurrentSQRT;
	    maxDuty = maxVolts * (fetPeriod / avgVolts);

	    if (duty > maxDuty)
		fetActualDutyCycle = maxDuty;
	    else
		fetActualDutyCycle = duty;
	}
	// otherwise, use PID - less accurate, lower performance
	else {
	    fetActualDutyCycle += fetPeriod * (RUN_MAX_DUTY_INCREASE * 0.01f);
	    if (fetActualDutyCycle > duty)
		fetActualDutyCycle = duty;
	    fetActualDutyCycle = runCurrentPID(fetActualDutyCycle);
	}
    }
    else {
	fetActualDutyCycle = duty;
    }

    _fetSetDutyCycle(fetActualDutyCycle);
}

void SysTick_Handler(void) {
    // reload the hardware watchdog
    runFeedIWDG();

    avgVolts = adcAvgVolts * ADC_TO_VOLTS;
    avgAmps = (adcAvgAmps - adcAmpsOffset) * adcToAmps;
    maxAmps = (adcMaxAmps - adcAmpsOffset) * adcToAmps;

    runWatchDog();

    idlePercent = 100.0f * (idleCounter-oldIdleCounter) * minCycles / totalCycles;
    oldIdleCounter = idleCounter;
    totalCycles = 0;

    runRpm();

    runThrotLim(fetDutyCycle);

    if (commandMode == CLI_MODE)
	cliCheck();
    else
	binaryCheck();

    runMilis++;
}

void PVD_IRQHandler(void) {
    // voltage dropping too low
    if (EXTI_GetITStatus(EXTI_Line16) != RESET) {
	// shut everything down
	runDisarm(REASON_LOW_VOLTAGE);

	// turn on both LEDs
	digitalLo(statusLed);
	digitalLo(errorLed);

	EXTI_ClearITPendingBit(EXTI_Line16);
    }
}

void runSetConstants(void) {
    int32_t startupMode = (int)p[STARTUP_MODE];
    float maxCurrent = p[MAX_CURRENT];

    if (startupMode < 0 || startupMode >= NUM_RUN_MODES)
	startupMode = 0;

    if (maxCurrent > RUN_MAX_MAX_CURRENT)
	maxCurrent = RUN_MAX_MAX_CURRENT;
    else if (maxCurrent < RUN_MIN_MAX_CURRENT)
	maxCurrent = RUN_MIN_MAX_CURRENT;

    runRPMFactor = (1e6f * (float)TIMER_MULT * 120.0f) / (p[MOTOR_POLES] * 6.0f);
    maxCurrentSQRT = sqrtf(maxCurrent);

    p[MOTOR_POLES] = (int)p[MOTOR_POLES];
    p[STARTUP_MODE] = startupMode;
    p[MAX_CURRENT] = maxCurrent;
}