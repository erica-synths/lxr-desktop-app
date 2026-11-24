/*
 * Copyright (C) 2012-2026 Erica Synths
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of the LXR firmware. It is licensed under the GNU General
 * Public License version 3, with the additional permission and terms in
 * NOTICE.md. See LICENSE and NOTICE.md in the root of the repository.
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fatfs.h"


//used for bootloader update.
//If 1, the main firmware will load a bootloader.img file to the bootloader section from SD card
#define UPDATE_BOOTLOADER 0

#if UPDATE_BOOTLOADER
#warning "UPDATE_BOOTLOADER is enabled: this firmware removes the bootloader's write protection and overwrites the bootloader from the SD card at boot. Never publish it as a normal firmware update; an interrupted or wrong bootloader update can only be repaired with a debug probe."
#include "flash_if.h"

typedef struct ImageFileHeaderStruct
{
	char fileHeader[8];		// ("LXRV2IMG") 8 bytes
	uint32_t datasize;		// size of the data blob, 4 bytes
	char checksum;			// 1 byte checksum
	char unused[3];			// 3 bytes padding to make header 16 byte long

} ImageFileHeader;

#define HEADER_SIZE sizeof(ImageFileHeader)
#endif

/* Private includes ----------------------------------------------------------*/
#include "usb_device.h"
#include "lcd.h"
#include "menu/menu.h"
#include "IO/DIO.h"
#include "menu/screensaver.h"
#include "menu/copyClearTools.h"
#include "ledHandler.h"
#include "buttonHandler.h"
#include "encoder.h"
#include "IO/adcPots.h"
#include "Preset/PresetManager.h"
#include "Soundengine/Soundengine.h"
#include "Soundengine/DSPAudio/random.h"
#include "Soundengine/USB/usb_manager.h"
#include "Soundengine/MIDI/Uart.h"

/* Private typedef -----------------------------------------------------------*/


/* Private define ------------------------------------------------------------*/
// The CMake build passes FIRMWARE_VERSION from project(VERSION); this is the fallback for STM32CubeIDE.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION " 2.00"
#endif
//#define f_unmount(path) f_mount(0, path, 0)

/* Private macro -------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;

I2S_HandleTypeDef hi2s2;
I2S_HandleTypeDef hi2s3;
DMA_HandleTypeDef hdma_spi2_tx;
DMA_HandleTypeDef hdma_spi3_tx;

RNG_HandleTypeDef hrng;

SD_HandleTypeDef hsd1;
DMA_HandleTypeDef hdma_sdmmc1_rx;
DMA_HandleTypeDef hdma_sdmmc1_tx;

SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim14;

UART_HandleTypeDef huart3;

PCD_HandleTypeDef hpcd_USB_OTG_FS;

/* USER CODE BEGIN PV */
float debugWatch = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2S2_Init(void);
static void MX_I2S3_Init(void);
static void MX_SPI1_Init(void);
static void MX_RNG_Init(void);
static void MX_TIM14_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_USB_OTG_FS_PCD_Init(void);
static void MX_TIM2_Init(void);
static void MX_SDMMC1_SD_Init(void);

/* Private user code ---------------------------------------------------------*/
void sd_formatRoutine()
{

	lcd_process_display_interrupt = 0;
	//menu to ask user to format card
	lcd_clear();
	lcd_home();
	lcd_string("Format card? All");
	//goto 2nd line
	lcd_setcursor(0,2);
	lcd_string("data erased?");

	while(encode_readButton()==0);
	while(encode_readButton()==1);

	lcd_clear();
	lcd_home();
	lcd_string("Really Format");
	//goto 2nd line
	lcd_setcursor(0,2);
	lcd_string("card?");

	while(encode_readButton()==0);

	//make fs
	FRESULT res;
	FATFS fs;           /* Filesystem object */
	BYTE work[512]; /* Work area (larger is better for processing time) */


	lcd_clear();
	lcd_home();
	lcd_string("Formatting card");
	//goto 2nd line
	lcd_setcursor(0,2);
	lcd_string("...");

	res = f_mkfs("", FM_FAT32, 0, work, 512);

	if(res!=FR_OK)
	{
		//error, can't format card
		lcd_clear();
		lcd_home();
		lcd_string("Error-Please try");
		//goto 2nd line
		lcd_setcursor(0,2);
		lcd_string("another card!");
		while(1);
	}
	else
	{
		/* Gives a work area to the default drive */
		f_mount(&fs, "", 0);

		//create glo.cfg file
		preset_createGlobals();

		//load init sound
		preset_init(0);
		//save init sound to card

		sprintf(savefileNames[0][PROJECT_NAMES], "Init");
		//preset_saveDrumsetSD(0);
		preset_saveProject(0);

		//success
		lcd_clear();
		lcd_home();
		lcd_string("Success!");
		//goto 2nd line
		lcd_setcursor(0,2);
		lcd_string("Please reboot.");

		while(1);
	}
}
/*
void sd_checkTempFiles()
{
	FRESULT res;
	FILINFO fno;
	res = f_stat("T000.PAT", &fno);

	if(res == FR_NO_FILE)
	{
		//create missing files
		copyClear_clearCurrentPattern();
		preset_savePatternTemp(0);
	}

	res = f_stat("T001.PAT", &fno);

	if(res == FR_NO_FILE)
	{
		//create missing files
		copyClear_clearCurrentPattern();
		preset_savePatternTemp(1);
	}

	res = f_stat("T002.PAT", &fno);

	if(res == FR_NO_FILE)
	{
		//create missing files
		copyClear_clearCurrentPattern();
		preset_savePatternTemp(2);
	}

	res = f_stat("T003.PAT", &fno);

	if(res == FR_NO_FILE)
	{
		//create missing files
		copyClear_clearCurrentPattern();
		preset_savePatternTemp(3);
	}
}
*/

__attribute__((section(".itcm_text"))) float foo(float a)
{
	return ((a + 105)*a + 945) / ((15*a + 420)*a + 945);
}

volatile static int8_t checkMainLoc = 0;

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
	/* Load functions into ITCM RAM */
	extern const unsigned char _itcm_text_start;
	extern const unsigned char _itcm_text_end;
	extern const unsigned char _itcm_data;
	memcpy((void*)&_itcm_text_start, &_itcm_data, (int) (&_itcm_text_end - &_itcm_text_start));

	float foobar = 0;
	for(int i=0;i<1024;i++)
	{
		foobar += foo(1.8*i);
	}

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /*
   * In the original code, the systick_timer was changed to 0.25ms
   * No idea how and if HALL allows to change the systick frequency.
   * seems limited to 1ms with HAL_GetTick()
   *
   * So TIM14 is set to a 0.24ms interrupt to increase the old systick_timer variable here.
   */

  //Spread Spectrum Clock Generation
  //RCC_SSCGR |= INCSTEP
  /* Enable/Configure Spread Spectrum, when PLL isn't running */
  //TODO: check if enabled and working!
  uint16_t INCSTEP = 56;
  uint16_t MODPER = 500;
  /*
  RCC->SSCGR = (1 << 31) | // Enable
  (0 << 30) | // Center
  (INCSTEP << 13) |
  (MODPER << 0);
  */

  //has to be written before PLL is activated
  //enabled, center,
  RCC->SSCGR = RCC_SSCGR_SSCGEN | (0 << RCC_SSCGR_SPREADSEL_Pos) |  (INCSTEP << RCC_SSCGR_INCSTEP_Pos) | (MODPER << RCC_SSCGR_MODPER_Pos) ;

  //--------------------------------


  /* Configure the system clock */
  SystemClock_Config();

  DWT_Delay_Init();
  //SysTick_Config(SystemCoreClock / 1000);
  //HAL_InitTick(TICK_INT_PRIORITY);

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_I2S2_Init();
  MX_I2S3_Init();
  MX_SPI1_Init();
  MX_FATFS_Init();
  MX_RNG_Init();
  MX_TIM14_Init();
  MX_USART3_UART_Init();
  MX_USB_OTG_FS_PCD_Init();
  MX_TIM2_Init();
  MX_SDMMC1_SD_Init();
  /* USER CODE BEGIN 2 */
  MX_USB_DEVICE_Init();
  dio_init(&hspi1);





  lcd_init();
  lcd_clear();
  lcd_home();
 // usb_init();




 
  menu_init();
  //HAL_ADC_Start(&hadc1);




  //we have to read out the pots once to have a defined state before loading the preset data
  HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buf, ADC_BUF_LEN);
  adc_checkPots();
  adc_checkPots();

  MX_FATFS_ReInit();

#if UPDATE_BOOTLOADER
  //print boot up message
  lcd_clear();
  lcd_string("Updating");
  //goto 2nd line
  lcd_setcursor(0,2);
  lcd_string("Bootloader...");

#else
	//print boot up message
lcd_clear();
lcd_string("Sonic Potions");
//goto 2nd line
lcd_setcursor(0,2);
lcd_string("LXR2 Drums V");
lcd_string(FIRMWARE_VERSION);
#endif

lcd_showSplash();


uint8_t sd_card_ok = 0;
//---- mount sd card ----  //todo move to preset manager or other matching .c file
//FatFs[0] = 0;
FRESULT res = f_mount((FATFS*)&SDFatFS, (TCHAR const*)SDPath,1);
if( res != FR_OK)
{

	lcd_home();

	//uint8_t error = 1;
	switch(res)
	{
		// There is no valid FAT volume on the drive or wrong lower layer implementation.
		case FR_NO_FILESYSTEM:
			lcd_clear();
			lcd_string("SD error:");
			//goto 2nd line
			lcd_setcursor(0,2);
			lcd_string("No FS found!");
			HAL_Delay(1000);
			sd_formatRoutine();
			break;

		// The lower layer, disk_read, disk_write or disk_ioctl function, reported that an unrecoverable hard error occured.
		case FR_DISK_ERR:
			lcd_clear();
			lcd_string("SD error:");
			//goto 2nd line
			lcd_setcursor(0,2);
			lcd_string("Disk error!");
			HAL_Delay(1000);
			break;

		/*The lower layer, disk_initialize function, reported that the storage device could not be got ready to work. One of the following possibilities is suspected.

		No medium in the drive.
		Wrong lower layer implementation.
		Wrong hardware configuration.
		The storage device has broken.
		*/
		case FR_NOT_READY:
			lcd_clear();
			lcd_string("SD error:");
			//goto 2nd line
			lcd_setcursor(0,2);
			lcd_string("No Card!");
			HAL_Delay(1000);
			break;



		default:
			lcd_clear();
			lcd_string("SD error:");
			//goto 2nd line
			lcd_setcursor(0,2);
			lcd_string("unknown!");
			HAL_Delay(1000);
			break;

	}



}
else
{
	sd_card_ok = 1;
}

//we have enough memory now, no more need for those stupid temp patterns!
//check if temp pattern files exist
//if(sd_card_ok) sd_checkTempFiles();

HAL_Delay(1000);
lcd_clear();

adc_checkPots();
adc_checkPots();

//-----------------------

sprintf(currentFolder, "/proj00");
if(sd_card_ok)
{




	FRESULT res;
	FILINFO fno;

	res = f_stat(currentFolder, &fno);
	if(res == FR_NO_FILE)
	{
		res = f_mkdir(currentFolder);
		if(res == FR_OK)
		{
			f_chdir(currentFolder);
		}
	}
	else
	{
		f_chdir(currentFolder);
	}
}

	soundengine_init();

	if(sd_card_ok)
	{
		if(!preset_loadGlobals())
		{
			lcd_clear();
			lcd_home();
			lcd_string(PSTR("glo.cfg error"));
			lcd_setcursor(0,2);
			lcd_string("Using defaults.");
			preset_createGlobals();
			HAL_Delay(1000);

		}
	}

	//load init preset
	if(sd_card_ok)
	{



		f_chdir(currentFolder);

#if 1

#if LOAD_PROJECT_ON_BOOT


#else
		//load all 64 kits from sd card
		lcd_clear();
		lcd_home();
		lcd_string(PSTR("Loading kits"));
		lcd_setcursor(0,2);
		lcd_string("from SD card");

		for(uint8_t i=0;i<NUM_KIT_SLOTS;i++)
		{
			preset_loadDrumsetSD(i,0);
		}
#endif

		//init morph target to peset 0
		morphParameters = mainParameters[0];

#if LOAD_PROJECT_ON_BOOT

#else
		//init to preset 1
		preset_loadDrumsetRam(0,0, 1);
#endif
#else
		if(!preset_loadDrumsetSD(0,0)) {
			lcd_clear();
			lcd_home();
			lcd_string(PSTR("Kit read error"));
			HAL_Delay(1000);
			preset_sendDrumsetParameters(); // send initial parameters (all 0's)
		}
#endif
	}

  //initialize empty pattern
if(sd_card_ok)
{

#if UPDATE_BOOTLOADER
	typedef  void (*pFunction)(void);
			  pFunction Jump_To_Application;
			  uint32_t JumpAddress;
			 // FIL firmwareFile;		/* place to hold 1 file*/

			  HAL_Delay(100);

			  //--------------------------------------------------------------------
			  //
			  //--------------------------------------------------------------------
			    FLASH_OBProgramInitTypeDef pOBInit;
			    HAL_FLASHEx_OBGetConfig(&pOBInit);

			    if(!(OB_WRP_SECTOR_0 & pOBInit.WRPSector))
			    {

			    	lcd_clear();
					lcd_home();
					lcd_string(PSTR("unlock sec 1..."));
					HAL_Delay(1000);

			  	  //sector 0 write protected -> unlock it
			  	  pOBInit.OptionType = OPTIONBYTE_WRP;

			  	  pOBInit.WRPState = OB_WRPSTATE_DISABLE;
			  	  pOBInit.WRPSector = OB_WRP_SECTOR_0;

			  	  // unlock FLASH in general
			  	  if(HAL_FLASH_Unlock() == HAL_OK) {
			  	     // unlock option bytes in particular
			  	     if(HAL_FLASH_OB_Unlock() == HAL_OK) {
			  	        // erase option bytes before programming

			  	           // program selected option byte
			  	            if(HAL_FLASHEx_OBProgram(&pOBInit) != HAL_OK)
			  	            {
			  	              HAL_FLASH_OB_Lock();
			  	              HAL_FLASH_Lock();

			  	            }
			  	            else
			  	            {
			  					HAL_FLASH_OB_Launch();
			  					HAL_FLASH_OB_Lock();
			  					HAL_FLASH_Lock();
			  	            }
			  	     }
			  	  }
			    }
			    //--------------------------------------------------------------------

				//start bootloader
		    	lcd_clear();
							lcd_home();
				lcd_string("update 2/3...");
				HAL_Delay(500);


				ImageFileHeader imageHeader;
				FRESULT fr;     /* Return value */

				 DIR dj;         /* Directory object */
				FILINFO fno;    /* File information */




				//open bootloader.img
				f_chdir("/");

				//create bootloader start file
			//	fr = f_open((FIL*)&SDFile,"REBOOT.CMD",FA_CREATE_NEW | FA_WRITE);
			//	f_close((FIL*)&SDFile);
				//res = f_open((FIL*)&SDFile,filename,FA_CREATE_ALWAYS | FA_WRITE);
				fr = f_open((FIL*)&SDFile,"BOOTLO~1.IMG",FA_OPEN_EXISTING | FA_READ);
				if(fr!=FR_OK)
				{
					//error
					lcd_clear();
					lcd_home();
					lcd_string("Can't open");
					lcd_setcursor(0,2);
					lcd_string("LXR*.img!");
					while(1);
				}

				//get filesize in bytes
				FSIZE_t filesize = f_size((FIL*)&SDFile);

				lcd_clear();
				lcd_home();
				lcd_string("Checking");
				lcd_setcursor(0,2);
				lcd_string("Firmware...");

				//read file header
				UINT bytesRead;
				f_read((FIL*)&SDFile, (void*)&imageHeader, HEADER_SIZE, &bytesRead);

				//header sanity check
				if(strncmp(imageHeader.fileHeader, "LXRV2IMG", (unsigned int)8) != 0)
				{
					//error -> filetype header tag does not match!
					lcd_clear();
					lcd_home();
					lcd_string("LXR*.img");
					lcd_setcursor(0,2);
					lcd_string("Not Valid!");
					while(1);
				}

				//Filesize check
				filesize = imageHeader.datasize;
				if( (filesize <= 0) || (filesize >= (ADDR_FLASH_SECTOR_1)))
				{
					lcd_clear();
					lcd_home();
					lcd_string("bootloader.img");
					lcd_setcursor(0,2);
					lcd_string("Wrong Size!");
					while(1);
				}

				//checksum calculation
				uint8_t sum = 0;
				for(uint32_t i=0; i<filesize; i++)
				{
					char byte;
					f_read((FIL*)&SDFile, (void*)&byte, 1, &bytesRead);
					sum += byte;
					if(bytesRead==0) break;
				}
				sum += imageHeader.checksum;

				if(sum != 0xff)
				{
					lcd_clear();
					lcd_home();
					lcd_string("Wrong");
					lcd_setcursor(0,2);
					lcd_string("Checksum!");
					while(1);
				}

				//rewind to firmware datablob
				f_lseek((FIL*)&SDFile, (FSIZE_t)HEADER_SIZE );
				//proceed to load image to flash

				//------- flash new bootloader ------------
				FLASH_If_Init();

				uint32_t SectorError;
				FLASH_EraseInitTypeDef eraseInit;
				eraseInit.TypeErase 	= FLASH_TYPEERASE_SECTORS;
				eraseInit.Sector 		=  FLASH_SECTOR_0;
				eraseInit.NbSectors	 	= 1; //sectors 1-7
				eraseInit.VoltageRange 	= FLASH_VOLTAGE_RANGE_3;


				HAL_FLASHEx_Erase(&eraseInit, &SectorError);

				if(SectorError != 0xFFFFFFFF)
				{
					lcd_clear();
					lcd_home();
					lcd_string("Can't erase");
					lcd_setcursor(0,2);
					lcd_string("Flash!");
					while(1);
				}


				lcd_clear();
				lcd_home();
				lcd_string("Loading Firmware");
				lcd_setcursor(0,2);
				lcd_string("0%");

	#define DATA_BUFFER_SIZE 16

			uint32_t bytesWritten=0;
			uint32_t data[DATA_BUFFER_SIZE];
			uint8_t eof_reached = 0;

			while(!eof_reached)
			{
				f_read((FIL*)&SDFile, data, DATA_BUFFER_SIZE*4, &bytesRead);

				if(bytesRead)
				{
					//program flash
					uint32_t addr = APPLICATION_ADDRESS+bytesWritten;

					//check address sanity
					if( (addr < APPLICATION_ADDRESS) || (addr >=(ADDR_FLASH_SECTOR_1)) )
					{
						lcd_clear();
						lcd_home();
						lcd_string("Address");
						lcd_setcursor(0,2);
						lcd_string("Error!");
						while(1);
					}
	#if !USE_NEW_FLASH_IF
					if(FLASH_If_Write((__IO uint32_t*)(&addr), data, bytesRead/4) != 0)
					{
						lcd_clear();
						lcd_home();
						lcd_string("Flash write");
						lcd_setcursor(0,2);
						lcd_string("Error!");
						while(1);
					}
	#else
					HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data);
	#endif


				}
				else
				{
					//file finished
					eof_reached=1;
				}

				//update Display
				bytesWritten += bytesRead;

				if(bytesWritten == filesize)
				{
					//datablob finished
					eof_reached=1;
				}
				//lcd_clear();
				//lcd_home();
				//lcd_string("Loading Firmware");
				char text[17];
				sprintf(text, "%d%%", (uint8_t)(bytesWritten/(float)filesize * 100.f));
				lcd_setcursor(0,2);
				lcd_string(text);
			}

			//delete LXR*.img and rename main.img to LXR_V2_V1.3b_17_02_2022
		 //   DIR dj;         /* Directory object */
		//    FILINFO fno;    /* File information */
		    fr = f_findfirst(&dj, &fno, "", "LXR*.img");  /* Start to search for LXR image */
		    if(fr == FR_OK && fno.fname[0])
		    {
		    	f_unlink(fno.fname);
		    	fr = f_rename("MAIN.IMG", "LXR_V2.IMG");
		    }



			//programming finished - reboot message
			lcd_clear();
			lcd_home();
			lcd_string("Please reboot");
			//goto 2nd line
			lcd_setcursor(0,2);
			lcd_string("+ hold encoder!");
			//NVIC_SystemReset();
			while(1);
#else

#if LOAD_PROJECT_ON_BOOT

#else
	for(uint8_t i=0;i<NUM_KIT_SLOTS;i++)
	{

		preset_loadPatternSD(i);
	}
	seq_copyPatternToBackupBuffer(0);
	led_updateSeqLeds();
#endif
	//load project + song names
	for(uint8_t i=0;i<NUM_KIT_SLOTS;i++)
	{
		preset_loadProjectNames(i);
	}
	//return to project 0
	f_chdir("/proj00");

#if LOAD_PROJECT_ON_BOOT
	uint8_t lastUsedProject;
	preset_loadLastUsedProjectNrFromSD(&lastUsedProject);
	//do not clean the tmp folder here, as we are just booting
	preset_loadProject(lastUsedProject, 0);
	if(res != FR_OK)
	{
		menu_showSdError(res);
	}
	else
	{
		menu_loadedPresetNr[CURRENT_PROJECT] = menu_selectedPresetNr[SAVE_TYPE_PROJECT] = lastUsedProject;
		//menu_resetSaveParameters();
	}
#else
	for(uint8_t i=0;i<NUM_KIT_SLOTS;i++)
	{
		preset_loadSongSD(i);
	}
	//if pattern change mode is auto load corresponding kit
	if(activeParameters[PAR_PATTERN_MODE] == 1)
	{
		preset_loadDrumsetRam(seq_patternSet.kitNr[seq_activePattern], 0, 1);
		menu_selectedPresetNr[SAVE_TYPE_KIT] = seq_patternSet.kitNr[seq_activePattern];
		menu_loadedPresetNr[CURRENT_KIT] = seq_patternSet.kitNr[seq_activePattern];
	}
#endif


}
else
{
  copyClear_clearCurrentPattern();
  preset_init(0);
  preset_loadDefaultGlobals();
}





//f_unmount((TCHAR const*)SDPath);



#if USE_FADER_VOLUME
 // HAL_ADC_Start_DMA(&hadc2, (uint32_t*)adc2_buf, ADC2_BUF_LEN);
#endif
  initMidiUart();
 // __HAL_UNLOCK(&huart3);
  //clear "Overrun error" flag if midi is received during boot
  __HAL_UART_CLEAR_OREFLAG(&huart3);


  __HAL_UART_ENABLE_IT(&huart3, UART_IT_RXNE);




  HAL_TIM_Base_Start_IT(&htim14); 	//our own own systick - prio 0 - higher prio comes first
#if CALC_SAMPLES_IN_INTERRUPT
#if USE_INTERRUPT_ONLY_DURING_SAVE_PROJ

#else
  HAL_TIM_Base_Start_IT(&htim2);	//calc sample interrupt - prio 1
#endif
#endif
 // HAL_TIM_Base_Start_IT(&htim13);


uint8_t lastSdState = BSP_SD_IsDetected();
buttonHandler_init(); //to make sure all leds are properly lit after boot

//make sure to have the tmp folder active
preset_initTmpFolder();
//----- load tmp stuff --------
//manually saved files
loadTmpFolderFiles();
//autosave kit
preset_LoadTmpKit();
//autosave pattern
preset_LoadTmpPat();
led_updateSeqLeds();

preset_resetAllSaveTimers();
//-----------------------------

lcd_process_display_interrupt = 1;

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI4_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(EXTI4_IRQn);

  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
  while (1)
  {
  checkMainLoc = -14;

	  //check SD card
	  if((BSP_SD_IsDetected() == SD_PRESENT) &&  (lastSdState == SD_NOT_PRESENT))
	  {
			MX_FATFS_ReInit();
			f_mount(0, (TCHAR const*)SDPath, 0); //un-mount
			memset((FATFS*)&SDFatFS,0,sizeof(FATFS)); //clear FS object to force reinit
			res = f_mount((FATFS*)&SDFatFS, (TCHAR const*)SDPath,1);
			if( res != FR_OK)
			{
				menu_showSdError( res);
			}
			else
			{
				//load project + song names
				for(uint8_t i=0;i<NUM_KIT_SLOTS;i++)
				{
					preset_loadProjectNames(i);
				}
			}

	  }
	  lastSdState = BSP_SD_IsDetected();

//	  soundengine_process();
	  //TODO clean up this multi call mess!!!!

    checkMainLoc = -13;
	//  menu_tick();
	  adc_checkPots();
	  soundengine_process();
	//  menu_tick();
    checkMainLoc = -12;
	  buttonHandler_tick();
	  adc_checkPots();
	//  menu_tick();
  checkMainLoc = -11;
	  //get the encoder values
	  menu_parseEncoder(encode_read4(),encode_readButton());
	//  menu_tick();
	  led_tickHandler();
	//  menu_tick();
	  adc_checkPots();
	  soundengine_process();
	//  menu_tick();
	  screensaver_check();
	//  menu_tick();
  checkMainLoc = -10;

	  //update the menu according to the encoder values
	  dio_process();
	//  menu_tick();
	  adc_checkPots();
    checkMainLoc = -9;
	//  menu_tick();
	  soundengine_process();
	//  menu_tick();
	  adc_checkFaders();
	//  menu_tick();
    checkMainLoc = -8;

	  //check if we have some usb midi messages and process them
	  MidiMsg msg;
	  if(usb_getMidi(&msg)) {
		  midiParser_parseMidiMessage(msg);
	  }

		//process midi on midi port
			uart_processMidi();

	  //check if we have to save something
	  preset_processTimer();

    checkMainLoc = -7;


    /* USER CODE END WHILE */
#endif
    /* USER CODE BEGIN 3 */
  }


  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 216;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }
  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_USART3|RCC_PERIPHCLK_SDMMC1
                              |RCC_PERIPHCLK_I2S|RCC_PERIPHCLK_CLK48;
  PeriphClkInitStruct.PLLI2S.PLLI2SN = 113;
  PeriphClkInitStruct.PLLI2S.PLLI2SP = RCC_PLLP_DIV2;
  PeriphClkInitStruct.PLLI2S.PLLI2SR = 5;
  PeriphClkInitStruct.PLLI2S.PLLI2SQ = 2;
  PeriphClkInitStruct.PLLI2SDivQ = 1;
  PeriphClkInitStruct.I2sClockSelection = RCC_I2SCLKSOURCE_PLLI2S;
  PeriphClkInitStruct.Usart3ClockSelection = RCC_USART3CLKSOURCE_PCLK1;
  PeriphClkInitStruct.Clk48ClockSelection = RCC_CLK48SOURCE_PLL;
  PeriphClkInitStruct.Sdmmc1ClockSelection = RCC_SDMMC1CLKSOURCE_CLK48;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */
  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV8;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_LEFT;
  hadc1.Init.NbrOfConversion = 14;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_8;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_9;
  sConfig.Rank = ADC_REGULAR_RANK_2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_14;
  sConfig.Rank = ADC_REGULAR_RANK_3;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_15;
  sConfig.Rank = ADC_REGULAR_RANK_4;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_12;
  sConfig.Rank = ADC_REGULAR_RANK_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_13;
  sConfig.Rank = ADC_REGULAR_RANK_6;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_10;
  sConfig.Rank = ADC_REGULAR_RANK_7;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_11;
  sConfig.Rank = ADC_REGULAR_RANK_8;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_5;
  sConfig.Rank = ADC_REGULAR_RANK_9;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_4;
  sConfig.Rank = ADC_REGULAR_RANK_10;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_3;
  sConfig.Rank = ADC_REGULAR_RANK_11;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_2;
  sConfig.Rank = ADC_REGULAR_RANK_12;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = ADC_REGULAR_RANK_13;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_14;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */


  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief I2S2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2S2_Init(void)
{

  /* USER CODE BEGIN I2S2_Init 0 */

  /* USER CODE END I2S2_Init 0 */

  /* USER CODE BEGIN I2S2_Init 1 */

  /* USER CODE END I2S2_Init 1 */
  hi2s2.Instance = SPI2;
  hi2s2.Init.Mode = I2S_MODE_MASTER_TX;
  hi2s2.Init.Standard = I2S_STANDARD_PHILIPS;
  hi2s2.Init.DataFormat = I2S_DATAFORMAT_16B;
  hi2s2.Init.MCLKOutput = I2S_MCLKOUTPUT_ENABLE;
  hi2s2.Init.AudioFreq = I2S_AUDIOFREQ_44K;
  hi2s2.Init.CPOL = I2S_CPOL_HIGH;
  hi2s2.Init.ClockSource = I2S_CLOCK_PLL;
  if (HAL_I2S_Init(&hi2s2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2S2_Init 2 */

  /* USER CODE END I2S2_Init 2 */

}

/**
  * @brief I2S3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2S3_Init(void)
{

  /* USER CODE BEGIN I2S3_Init 0 */

  /* USER CODE END I2S3_Init 0 */

  /* USER CODE BEGIN I2S3_Init 1 */

  /* USER CODE END I2S3_Init 1 */
  hi2s3.Instance = SPI3;
  hi2s3.Init.Mode = I2S_MODE_MASTER_TX;
  hi2s3.Init.Standard = I2S_STANDARD_PHILIPS;
  hi2s3.Init.DataFormat = I2S_DATAFORMAT_16B;
  hi2s3.Init.MCLKOutput = I2S_MCLKOUTPUT_ENABLE;
  hi2s3.Init.AudioFreq = I2S_AUDIOFREQ_44K;
  hi2s3.Init.CPOL = I2S_CPOL_HIGH;
  hi2s3.Init.ClockSource = I2S_CLOCK_PLL;
  if (HAL_I2S_Init(&hi2s3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2S3_Init 2 */

  /* USER CODE END I2S3_Init 2 */

}

/**
  * @brief RNG Initialization Function
  * @param None
  * @retval None
  */
static void MX_RNG_Init(void)
{

  /* USER CODE BEGIN RNG_Init 0 */

  /* USER CODE END RNG_Init 0 */

  /* USER CODE BEGIN RNG_Init 1 */

  /* USER CODE END RNG_Init 1 */
  hrng.Instance = RNG;
  if (HAL_RNG_Init(&hrng) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RNG_Init 2 */

  /* USER CODE END RNG_Init 2 */

}

/**
  * @brief SDMMC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SDMMC1_SD_Init(void)
{

  /* USER CODE BEGIN SDMMC1_Init 0 */

  /* USER CODE END SDMMC1_Init 0 */

  /* USER CODE BEGIN SDMMC1_Init 1 */

  /* USER CODE END SDMMC1_Init 1 */
  hsd1.Instance = SDMMC1;
  hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockBypass = SDMMC_CLOCK_BYPASS_DISABLE;
  hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_ENABLE;
  hsd1.Init.BusWide = SDMMC_BUS_WIDE_1B;
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
  hsd1.Init.ClockDiv = 1;
  /* USER CODE BEGIN SDMMC1_Init 2 */

  /* USER CODE END SDMMC1_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_HIGH;
  hspi1.Init.CLKPhase = SPI_PHASE_2EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 1;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 1500;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief TIM14 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM14_Init(void)
{

  /* USER CODE BEGIN TIM14_Init 0 */

  /* USER CODE END TIM14_Init 0 */

  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM14_Init 1 */
  //bus APB1 Timer clock = 108MHz
  //target frequency 0,000125s = 0,125 ms
  //0,000125 / (1/108000000) = 1350
  /* USER CODE END TIM14_Init 1 */
  htim14.Instance = TIM14;
  htim14.Init.Prescaler = 13500;
  htim14.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim14.Init.Period = 1;
  htim14.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim14.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim14) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_OC_Init(&htim14) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_TOGGLE;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_OC_ConfigChannel(&htim14, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM14_Init 2 */

  /* USER CODE END TIM14_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 31250;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief USB_OTG_FS Initialization Function
  * @param None
  * @retval None
  */
static void MX_USB_OTG_FS_PCD_Init(void)
{
  hpcd_USB_OTG_FS.Instance = USB_OTG_FS;
  hpcd_USB_OTG_FS.Init.dev_endpoints = 6;
  hpcd_USB_OTG_FS.Init.speed = PCD_SPEED_FULL;
  hpcd_USB_OTG_FS.Init.dma_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_OTG_FS.Init.Sof_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.lpm_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.vbus_sensing_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.use_dedicated_ep1 = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_OTG_FS) != HAL_OK)
  {
    Error_Handler();
  }

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Stream4_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);
  /* DMA1_Stream5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
  /* DMA2_Stream0_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
  /* DMA2_Stream3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
  /* DMA2_Stream6_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(Clock_Out_GPIO_Port, Clock_Out_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, DIO_LD_Pin|LED_43_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, LCD_D4_Pin|LCD_D5_Pin|LCD_D6_Pin|LCD_D7_Pin
                          |LCD_E_Pin|LCD_RS_Pin, GPIO_PIN_RESET);



  /*Configure GPIO pin : Clock_Out_Pin */
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);



  /*Configure GPIO pin : Clock_Out_Pin */
  GPIO_InitStruct.Pin = Clock_Out_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(Clock_Out_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : DIO_LD_Pin LED_43_Pin */
  GPIO_InitStruct.Pin = DIO_LD_Pin|LED_43_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : LCD_D4_Pin LCD_D5_Pin LCD_D6_Pin LCD_D7_Pin
                           LCD_E_Pin LCD_RS_Pin */
  GPIO_InitStruct.Pin = LCD_D4_Pin|LCD_D5_Pin|LCD_D6_Pin|LCD_D7_Pin
                          |LCD_E_Pin|LCD_RS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pins : Enc_A_Pin Enc_B_Pin */
  GPIO_InitStruct.Pin = Enc_A_Pin|Enc_B_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pin : Enc_Switch_Pin */
  GPIO_InitStruct.Pin = Enc_Switch_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(Enc_Switch_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : SDIO_CS_Pin */
  GPIO_InitStruct.Pin = SDIO_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(SDIO_CS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : SD_CD_Pin switch_l1_Pin switch_r1_Pin */
  GPIO_InitStruct.Pin = SD_CD_Pin|switch_l1_Pin|switch_r1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /*Configure GPIO pin : Clock_In_EXTI4_Pin */
  GPIO_InitStruct.Pin = Clock_In_EXTI4_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(Clock_In_EXTI4_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : Reset_In_EXTI5_Pin */
  GPIO_InitStruct.Pin = Reset_In_EXTI5_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(Reset_In_EXTI5_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : switch_l2_Pin switch_r2_Pin */
  GPIO_InitStruct.Pin = switch_l2_Pin|switch_r2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : SW_43_Pin */
  GPIO_InitStruct.Pin = SW_43_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(SW_43_GPIO_Port, &GPIO_InitStruct);

  // /* EXTI interrupt init*/
  // HAL_NVIC_SetPriority(EXTI4_IRQn, 3, 0);
  // HAL_NVIC_EnableIRQ(EXTI4_IRQn);

  // HAL_NVIC_SetPriority(EXTI9_5_IRQn, 3, 0);
  // HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */

  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     tex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
