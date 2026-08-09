/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * @brief             : 多任务架构 — SD_Task / Audio_Task / LCD_Task
  ******************************************************************************
  */
/* USER CODE END Header */

#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* USER CODE BEGIN Includes */
#include "lcd.h"
#include "sd_task.h"
#include "audio_task.h"
#include "lcd_task.h"
#include "uart_recv.h"
/* USER CODE END Includes */

/* USER CODE BEGIN Variables */
extern osSemaphoreId_t sdReqSem;
extern osSemaphoreId_t sdDoneSem;
extern osSemaphoreId_t lcdWakeSem;

static const osThreadAttr_t sdTask_attr = {
    .name = "SD_Task", .stack_size = 1024, .priority = osPriorityAboveNormal,
};
static const osThreadAttr_t audioTask_attr = {
    .name = "Audio_Task", .stack_size = 2048, .priority = osPriorityNormal,
};
static const osThreadAttr_t lcdTask_attr = {
    .name = "LCD_Task", .stack_size = 1024, .priority = osPriorityLow,
};
/* USER CODE END Variables */

osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 512,
  .priority = (osPriority_t) osPriorityLow,
};

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void)
{
  /* USER CODE BEGIN Init */
  /* USER CODE END Init */
  /* USER CODE BEGIN RTOS_SEMAPHORES */
  sdReqSem   = osSemaphoreNew(1, 0, NULL);
  sdDoneSem  = osSemaphoreNew(1, 0, NULL);
  lcdWakeSem = osSemaphoreNew(1, 0, NULL);
  /* USER CODE END RTOS_SEMAPHORES */
  /* USER CODE BEGIN RTOS_MUTEXES */
  sdMutex    = osMutexNew(NULL);  /* 串行化 sd_sync */
  /* USER CODE END RTOS_MUTEXES */
  /* USER CODE BEGIN RTOS_QUEUES */
  /* USER CODE END RTOS_QUEUES */

  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  osThreadNew(StartSDTask,    NULL, &sdTask_attr);
  osThreadNew(StartAudioTask, NULL, &audioTask_attr);
  osThreadNew(StartLCDTask,   NULL, &lcdTask_attr);
  /* USER CODE END RTOS_THREADS */
}

void StartDefaultTask(void *argument)
{
  (void)argument;

  /* LCD 初始化已由 LCD_Task 完成, defaultTask 负责串口命令轮询
     (每 50ms 检查一次, 支持 #UPDATE# #SEND# #INFO#) */
  for (;;)
  {
    UartRecv_Poll();
    osDelay(50);
  }
}
