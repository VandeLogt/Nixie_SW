//-----------------------------------------------------------------------------
// Created: 07/12/2011 15:17:35
// Author : Emile
// File   : Nixie.c
//-----------------------------------------------------------------------------
#include <avr/io.h>
#include <util/atomic.h>
#include "scheduler.h"
#include "usart.h"
#include "IRremote.h"
#include "Nixie.h"
#include "i2c.h"
#include "command_interpreter.h"
#include "dht22.h"
#include "eep.h"


bool		  test_nixies = 0;					// S3 command / IR #9 command
bool		  nixie_lifetimesaver = false;		// Toggle nixie's on (false), no LST or off (true), LST active - *3 IR remote
bool		  override_lifetimesaver = false;	// Override lifestimesaver when True - *2 IR remote 	
uint8_t		  cnt_60sec   = 0;					// 60 seconds counter during nixie_lts - *1 IR remote 	 	
uint8_t       cnt_50usec  = 0;					// 50 usec. counter
unsigned long t2_millis   = 0UL;				// msec. counter
uint16_t      dht22_hum   = 0;					// Humidity E-1 %
int16_t       dht22_temp  = 0;					// Temperature E-1 Celsius
//int16_t       dht22_dewp  = 0;				// Dewpoint E-1 Celsius
double        bmp180_pressure = 0.0;			// Pressure E-1 mbar
double        bmp180_temp     = 0.0;			// Temperature E-1 Celsius

bool          dst_active      = false;			// true = Daylight Saving Time active
uint8_t       blank_begin_h   = 0;
uint8_t       blank_begin_m   = 0;
uint8_t       blank_end_h     = 0;
uint8_t       blank_end_m     = 0;

extern bool time_only;				// Toggles between time and date only with no RGB to all task shown
extern bool default_rgb_pattern;	// Set RGB colour to a default pattern
extern bool random_rgb_pattern;		// Change per second the RGB colour in a random pattern
extern bool fixed_rgb_pattern;		// Change per second the RGB colour in a fix pattern	
extern bool display_60sec;			// Display time, date and sensor outputs for 60 sec.
extern uint8_t fixed_rgb_colour;

uint8_t       wheel_effect    = 0;    // 0: none, 1: from 59->00, 2: every second/minute
bool          display_time    = true; // true = hh:mm:ss is on display 
uint8_t		  col_time;       // Colour for Time display
uint8_t		  col_date;       // Colour for Date & Year display
uint8_t       col_temp;       // Colour for Temperature display
uint8_t       col_humi;       // Colour for Humidity display
uint8_t       col_dewp;       // Colour for Dew-point display
uint8_t       col_pres;       // Colour for Pressure display
uint8_t       col_roll;       // Colour for second roll-over
uint8_t       led_r[NR_LEDS]; // Array with 8-bit red colour for all WS2812
uint8_t       led_g[NR_LEDS]; // Array with 8-bit green colour for all WS2812
uint8_t       led_b[NR_LEDS]; // Array with 8-bit blue colour for all WS2812
bool          hv_relay_sw;    // switch for hv_relay
bool          hv_relay_fx;    // fix for hv_relay

extern char   rs232_inbuf[];       // RS232 input buffer



//---------------------------------------------------------------------------
// Copy to hardware PVB v0.21: MSB (bit 39) first --> LSB (bit 00)
//
// Bits 39..32: Decimal points:  -    -    -    -   LDP1 LDP2 LDP3 LDP4
// Bits 31..24: Decimal points: LDP5 LDP6 RDP1 RDP2 RDP3 RDP4 RDP5 RDP6
// Bits 23..16: Hours         : SLD  SLC  SLB  SLA  SHD  SHC  SHB  SHA 
// Bits 15..08: Minutes       : MLD  MLC  MLB  MLA  MHD  MHC  MHB  MHA
// Bits 07..00: Seconds       : HLD  HLC  HLB  HLA  HHD  HHC  HHB  HHA
//---------------------------------------------------------------------------
uint32_t nixie_bits  = 0UL;
uint8_t  nixie_bits8 = 0UL;
uint8_t  rgb_colour  = BLACK;

/*------------------------------------------------------------------
  Purpose  : This function returns the number of milliseconds since
			 power-up. It is defined in delay.h
  Variables: -
  Returns  : The number of milliseconds since power-up
  ------------------------------------------------------------------*/
unsigned long millis(void)
{
	unsigned long m;
	// disable interrupts while we read timer0_millis or we might get an
	// inconsistent value (e.g. in the middle of a write to timer0_millis)
	ATOMIC_BLOCK(ATOMIC_FORCEON)
	{
		m = t2_millis;
	}
	return m;
} // millis()

/*------------------------------------------------------------------
  Purpose  : This function waits a number of milliseconds. It is 
             defined in delay.h. Do NOT use this in an interrupt.
  Variables: 
         ms: The number of milliseconds to wait.
  Returns  : -
  ------------------------------------------------------------------*/
void delay_msec(uint16_t ms)
{
	unsigned long start = millis();

	while ((millis() - start) < ms) ;
} // delay_msec()

/*------------------------------------------------------------------------
  Purpose  : This is the Timer-Interrupt routine which runs every 50 usec. 
             (f=20 kHz). It is used by the task-scheduler and the IR-receiver.
  Variables: -
  Returns  : -
  ------------------------------------------------------------------------*/
ISR(TIMER2_COMPA_vect)
{
	PORTB |= TIME_MEAS; // Time Measurement
	if (++cnt_50usec > 19) 
	{
		scheduler_isr(); // call the ISR routine for the task-scheduler
		t2_millis++;     // update millisecond counter
		cnt_50usec = 0;
	} // if	
	ir_isr();            // call the ISR routine for the IR-receiver
	PORTB &= ~TIME_MEAS; // Time Measurement
} // ISR()


/*-----------------------------------------------------------------------------
  Purpose  : This routine fills the led_<x>[NR_LEDS] array.
  Variables: ws2812b_red, ws2812b_green, ws2812b_blue : the byte to fill
  Returns  : -
  ---------------------------------------------------------------------------*/
void ws2812b_fill_rgb(uint8_t ws2812b_red, uint8_t ws2812b_green, uint8_t ws2812b_blue)
{
	uint8_t i;
	
	for (i = 0; i < NR_LEDS; i++)
	{
		led_g[i] = ws2812b_green;
		led_r[i] = ws2812b_red;
		led_b[i] = ws2812b_blue;
	} // for i
	
	ws2812_task();	
}


/*-----------------------------------------------------------------------------
  Purpose  : This routine sends one byte to the WS2812B LED-string.
  Variables: bt: the byte to send
  Returns  : -
  ---------------------------------------------------------------------------*/
void ws2812b_send_byte(uint8_t bt)
{
    uint8_t i,x = 0x80; // Start with MSB first
    
    for (i = 0; i < 8; i++)
    {
        if (bt & x)
        {    // Send a 1   
             ws2812b_send_1;
        } // if
        else 
        {   // Send a 0
            ws2812b_send_0;
        } // else
        x >>= 1; // Next bit
    } // for i
} // ws2812b_send_byte()

/*-----------------------------------------------------------------------------
  Purpose  : This routine sends the RGB-bytes for every LED to the WS2812B
             LED string.
  Variables: 
      led_g: the green byte array
      led_r: the red byte array
      led_b: the blue byte array
  Returns  : -
  ---------------------------------------------------------------------------*/
void ws2812_task(void)
{
    uint8_t i;

    cli(); // disable IRQ for time-sensitive LED-timing
    for (i = 0; i < NR_LEDS; i++)
    {
        ws2812b_send_byte(led_g[i]); // Send one byte of Green
        ws2812b_send_byte(led_r[i]); // Send one byte of Red
        ws2812b_send_byte(led_b[i]); // Send one byte of Blue
    } // for i
    sei(); // enable IRQ again
} // ws2812_task()

/*------------------------------------------------------------------------
  Purpose  : This task is called every time the time is displayed on the
             Nixies. It checks for a change from summer- to wintertime and
			 vice-versa.
			 To start DST: Find the last Sunday in March: @2 AM advance clock to 3 AM.
			 To stop DST : Find the last Sunday in October: @3 AM set clock back to 2 AM (only once!).
  Variables: p: pointer to time-struct
  Returns  : -
  ------------------------------------------------------------------------*/
void check_and_set_summertime(Time p)
{
	uint8_t        day,lsun03,lsun10;
    static uint8_t advance_time = 0;
	static uint8_t revert_time  = 0;
	char           s[20];
	
	if (p.mon == 3)
	{
		day    = ds3231_calc_dow(31,3,p.year); // Find day-of-week for March 31th
		lsun03 = 31 - (day % 7);               // Find last Sunday in March
#ifdef DEBUG_SENSORS
		sprintf(s,"lsun03=%d\n",lsun03); xputs(s);
#endif
		switch (advance_time)
		{
			case 0: if ((p.date == lsun03) && (p.hour == 2) && (p.min == 0))
					{   // At 2:00 AM advance time to 3 AM, check for one minute
						advance_time = 1;
					} // if
					else if (p.date < lsun03) dst_active = false;
					else if (p.date > lsun03) dst_active = true;
					else if (p.hour < 2)      dst_active = false;
					break;
			case 1: // Now advance time, do this only once
					ds3231_settime(3,0,p.sec); // Set time to 3:00, leave secs the same
					advance_time = 2;
					dst_active   = true;
					break;
			case 2: if (p.min > 0) advance_time = 0; // At 3:01:00 back to normal
					dst_active = true;
					break;
		} // switch
	} // if
	else if (p.mon == 10)
	{
		day    = ds3231_calc_dow(31,10,p.year); // Find day-of-week for October 31th
		lsun10 = 31 - (day % 7);                // Find last Sunday in October
#ifdef DEBUG_SENSORS
		sprintf(s,"lsun10=%d\n",lsun10); xputs(s);
#endif
		switch (revert_time)
		{
			case 0: if ((p.date == lsun10) && (p.hour == 3) && (p.min == 0))
					{   // At 3:00 AM revert time back to 2 AM, check for one minute
						revert_time = 1;
					} // if
					else if (p.date > lsun10) dst_active = false;
					else if (p.date < lsun10) dst_active = true;
					else if (p.hour < 3)      dst_active = true;
					break;
			case 1: // Now revert time, do this only once
					ds3231_settime(2,0,p.sec); // Set time back to 2:00, leave secs the same
					revert_time = 2;
					dst_active  = false;
					break;
			case 2: // make sure that we passed 3 AM in order to prevent multiple reverts
					if (p.hour > 3) revert_time = 0; // at 4:00:00 back to normal
					dst_active = false;
					break;
		} // switch
	} // else if
	else if ((p.mon < 3) || (p.mon > 10)) dst_active = false;
	else                                  dst_active = true; 
} // check_and_set_summertime()

/*------------------------------------------------------------------------
  Purpose  : This task is called by the Task-Scheduler every 5 seconds.
             It reads the DHT22 sensor Humidity and Temperature
  Variables: dht22_humidity, dht22_temperature
  Returns  : -
  ------------------------------------------------------------------------*/
void dht22_task(void)
{
	int8_t x;
	
	dht22_read(&dht22_hum,&dht22_temp); // read DHT22 sensor
	//dht22_dewp = dht22_dewpoint(dht22_hum,dht22_temp);
#ifdef DEBUG_SENSORS
	char s[30];
	x = dht22_hum / 10;
	sprintf(s,"dht22: RH=%d.%d %%, \n",x,dht22_hum-10*x); 
	xputs(s);
	x = dht22_temp / 10;
	//sprintf(s," T=%d.%d �C,",x,dht22_temp-10*x);
	//xputs(s);
	//x = dht22_dewp / 10;
	//sprintf(s," dewpoint=%d.%d �C\n",x,dht22_dewp-10*x);
	//xputs(s);
#endif
} // dht22_task()

/*------------------------------------------------------------------------
  Purpose  : This task is called by the Task-Scheduler every second.
             It reads the BMP180 pressure and temperature.
  Variables: bmp180_pressure, bmp180_temperature
  Returns  : -
  ------------------------------------------------------------------------*/
void bmp180_task(void)
{
} // bmp180_task()

/*------------------------------------------------------------------------
  Purpose  : This task is called by the Task-Scheduler every 50 msec. 
             It updates the Nixie tubes with the new bits.
  Variables: nixie_bits (32 bits)
  Returns  : -
  ------------------------------------------------------------------------*/
void update_nixies(void)
{
	uint8_t         i, x;
	uint32_t        mask;        // start with MSB
	uint8_t         mask8;
	uint32_t        bitstream;   // copy of nixie_bits
	uint8_t         bitstream8;  // copy of nixie_bits8
	static uint8_t  wheel_cnt_sec = 0, wheel_cnt_min = 0;
	static uint8_t  bits_min_old, bits_sec_old;
	uint8_t         wheel[WH_MAX] = {11,22,33,44,55,66,77,88,99,0};
			
	//PORTB &= ~(RGB_R | RGB_G | RGB_B);               // clear  LED colours
	//PORTB |= (rgb_colour & (RGB_R | RGB_G | RGB_B)); // update LED colours
	
	//bitstream = nixie_bits; // copy original bitstream for PCB < v0.21
	// Needed for PCB hardware v0.21 and higher
	// nixie_bits: LDP RDP HH HL MH ML SH SL
	// hardware  : LDP RDP SL SH ML MH HL HH
	bitstream  = (nixie_bits & 0xFF000000); // copy LDP and RDP bits
	bitstream |= (nixie_bits & 0x0000000F) << 20; // SL
	bitstream |= (nixie_bits & 0x000000F0) << 12; // SH
	bitstream |= (nixie_bits & 0x00000F00) <<  4; // ML
	bitstream |= (nixie_bits & 0x0000F000) >>  4; // MH
	bitstream |= (nixie_bits & 0x000F0000) >> 12; // HL
	bitstream |= (nixie_bits & 0x00F00000) >> 20; // HH
	
	bitstream8 = nixie_bits8; // copy original bitstream
	if (display_time)
	{
		//------------------------------------------
		// Rotate the MINUTES digits (wheel-effect)
		//------------------------------------------
		x = (uint8_t)((nixie_bits & 0x0000FF00) >> 8); // isolate minutes digits
		switch (wheel_effect)
		{
			case 0: // no wheel-effect
					wheel_cnt_min = 0;
					break;
			case 1: // wheel-effect from 59 -> 0
					if (x == 0)
					{	// minutes == 0, wheel-effect
						bitstream |= (((uint32_t)wheel[wheel_cnt_min]) << 8);
						if (++wheel_cnt_min > WH_MAX-1) wheel_cnt_min = WH_MAX-1;
					} // if
					else wheel_cnt_min = 0; // reset for next wheel-effect
					break;
			case 2: // wheel-effect on every change in minutes
					if (x != bits_min_old)
					{	// change in minutes
						bitstream   &= 0xFFFF00FF; // clear minutes bits
						bitstream   |= (((uint32_t)wheel[wheel_cnt_min]) << 8);
						if (++wheel_cnt_min > WH_MAX-1)
						{
							wheel_cnt_min = WH_MAX-1;
							bits_min_old = x;
						} // if
					} // if
					else wheel_cnt_min = 0; // reset for next minute
					break;
		} // switch
		//------------------------------------------
		// Rotate the SECONDS digits (wheel-effect)
		//------------------------------------------
		x = (uint8_t)((bitstream & 0x00FF0000) >> 16); // isolate seconds digits
		switch (wheel_effect)
		{
			case 0: // no wheel-effect
			wheel_cnt_sec = 0;
			break;
			case 1: // wheel-effect from 59 -> 0
				if (x == 0)
				{	// seconds == 0, wheel-effect
					bitstream |= (((uint32_t)wheel[wheel_cnt_sec]) << 16);
					if (++wheel_cnt_sec > WH_MAX-1) wheel_cnt_sec = WH_MAX-1;
				} // if
				else wheel_cnt_sec = 0; // reset for next second
				break;
			case 2: // wheel-effect on every change in seconds
				if (x != bits_sec_old)
				{	// change in seconds
					bitstream &= 0x00FF0000; // clear seconds bits
					bitstream |= (((uint32_t)wheel[wheel_cnt_sec]) << 16);
					if (++wheel_cnt_sec > WH_MAX-1)
					{
						wheel_cnt_sec = WH_MAX-1;
						bits_sec_old = x;
					} // if
				} // if
				else wheel_cnt_sec = 0; // reset for next second
			break;
		} // switch
	} // if	
	
	//--------------------------------------------------
	// Now send the first 8 nixie-bits to hardware
	// Bit-order: X X X X LDP1 LDP2 LDP3 LDP4
	//--------------------------------------------------
	
	mask8 = 0x80;
	for (i = 0; i < 8; i++)
	{
		PORTD &= ~(SHCP|STCP); // set clocks to 0
		if ((bitstream8 & mask8) == mask8)
		PORTD |=  SDIN; // set SDIN to 1
		else PORTD &= ~SDIN; // set SDIN to 0
		mask8 >>= 1;     // shift right 1
		PORTD |=  SHCP; // set clock to 1
		PORTD &= ~SHCP; // set clock to 0 again
	}
	//--------------------------------------------------
	// Now send the remaining 32 nixie-bits to hardware
	//--------------------------------------------------
	mask = 0x80000000;
	for (i = 0; i < 32; i++)
	{
		PORTD &= ~(SHCP|STCP); // set clocks to 0
		if ((bitstream & mask) == mask)
			 PORTD |=  SDIN; // set SDIN to 1
		else PORTD &= ~SDIN; // set SDIN to 0
		mask >>= 1;     // shift right 1
		PORTD |=  SHCP; // set clock to 1
		PORTD &= ~SHCP; // set clock to 0 again
	} // for i
	// Now clock bits from shift-registers to output-registers
	PORTD |=  STCP; // set clock to 1
	PORTD &= ~STCP; // set clock to 0 again
} // update_nixies()

/*------------------------------------------------------------------------
  Purpose  : Encode a byte into 2 BCD numbers.
  Variables: x: the byte to encode
  Returns  : the two encoded BCD numbers
  ------------------------------------------------------------------------*/
uint8_t encode_to_bcd(uint8_t x)
{
	uint8_t temp;
	uint8_t retv = 0;
		
	temp   = x / 10;
	retv  |= (temp & 0x0F);
	retv <<= 4; // SHL 4
	temp   = x - temp * 10;
	retv  |= (temp & 0x0F);
	return retv;
} // encode_to_bcd()

/*------------------------------------------------------------------------
  Purpose  : clears one Nixie tube. The 74141/K155ID1 ICs blank for
             codes > 9. This is used here.
  Variables: nr: [1..6], the Nixie tube to clear. 1=most left, 6= most right.
  Returns  : -
  ------------------------------------------------------------------------*/
void clear_nixie(uint8_t nr)
{
	uint8_t shift;
		
	if (nr == 0)
	{
		//rgb_colour = BLACK;
		ws2812b_fill_rgb(BLACK, BLACK, BLACK);
		PORTC &= ~(0x0F);
		
		for (nr = 1; nr < 7; ++nr)
		{
 			shift = 24 - 4 * nr;
 			nixie_bits |= (NIXIE_CLEAR << shift);
		}
		nixie_bits &=0x00FFFFFF;		// Clear decimal point bits
	}
	else
	{
		shift = 24 - 4 * nr;
		nixie_bits |= (NIXIE_CLEAR << shift);
	} // if
} // clear_nixie()

/*------------------------------------------------------------------------
  Purpose  : To test the Nixie tubes, showing 0 to 9, %, C, P, and RGB Leds 
  Variables: -
  Returns  : -
  ------------------------------------------------------------------------*/
void ftest_nixies(void)
{
	static uint8_t std_test = 0;
	
	PORTC &=~(0x0F);
	
	
		switch (std_test)
		{
			case 0: nixie_bits = 0x01000000; std_test = 1; break;
			case 1: nixie_bits = 0x02111111; std_test = 2; break;
			case 2: nixie_bits = 0x40222222; PORTC |= (DEGREESYMBOL | LED_IN19A); std_test = 3; break;
			case 3: nixie_bits = 0x04333333; std_test = 4; break;
			case 4: nixie_bits = 0x08444444; std_test = 5; break;
			case 5: nixie_bits = 0x80555555; PORTC |= (HUMIDITYSYMBOL | LED_IN19A); std_test = 6; break;
			case 6: nixie_bits = 0x10666666; std_test = 7; break;
			case 7: nixie_bits = 0x20777777; std_test = 8; break;
			case 8: nixie_bits = 0xFF888888; PORTC |= (PRESSURESYMBOL | LED_IN19A); std_test = 9; break;
			case 9: nixie_bits = 0xFF999999; std_test = 0; break;
		} // switch
	
	//rgb_colour = std_test & 0x07;
	
} // ftest_nixies()

/*------------------------------------------------------------------------
  Purpose  : This functions is called when random_rgb == true.
			 Called every second.
  Variables: -
  Returns  : -
  ------------------------------------------------------------------------*/
void fixed_random_rgb_colour(uint8_t s, bool rndm)
{
	
	if (rndm == true )
	{
		s = rand() %60;
	}
	
	switch (s)
	{
		case 0:
		case 7:
		case 14:
		case 21:
		case 28:
		case 35:
		case 42:
		case 49:
		case 56: 
			//rgb_colour = WHITE;	
			ws2812b_fill_rgb(RED, GREEN, BLUE);
			break;
		case 1:
		case 8:
		case 15:
		case 22:
		case 29:
		case 36:
		case 43:
		case 50:
		case 57:	
			//rgb_colour = RED;
			ws2812b_fill_rgb(RED, BLACK, BLACK);
			break;
		case 2:
		case 9:
		case 16:
		case 23:
		case 30:
		case 37:
		case 44:
		case 51:
		case 58:
			//rgb_colour = GREEN;
			ws2812b_fill_rgb(BLACK, GREEN, BLACK);
			break;
		case 3:
		case 10:
		case 17:
		case 24:
		case 31:
		case 38:
		case 45:
		case 52:
		case 59:
			//rgb_colour = BLUE;
			ws2812b_fill_rgb(BLACK, BLACK, BLUE);
			break;
		case 4:
		case 11:
		case 18:
		case 25:
		case 32:
		case 39:
		case 46:
		case 53:
			//rgb_colour = YELLOW;
			ws2812b_fill_rgb(RED, GREEN, BLACK);
			break;
		case 5:
		case 12:
		case 19:
		case 26:
		case 33:
		case 40:
		case 47:
		case 54:
			//rgb_colour = MAGENTA;
			ws2812b_fill_rgb(RED, BLACK, BLUE);
			break;
		case 6:
		case 13:
		case 20:
		case 27:
		case 34:
		case 41:
		case 48:
		case 55:
			//rgb_colour = CYAN;
			ws2812b_fill_rgb(BLACK, GREEN, BLUE);
			break;
		default:
			//rgb_colour = BLACK;
			ws2812b_fill_rgb(BLACK, BLACK, BLACK);
			break;	
	} // Switch
} // fixed_random_rgb_colour

/*------------------------------------------------------------------------
  Purpose  : This function decides if the current time falls between the
             blanking time for the Nixies.
  Variables: p: Time struct containing actual time
  Returns  : true: blanking is true, false: no blanking
  ------------------------------------------------------------------------*/
bool blanking_active(Time p)
{
	if ( ((p.hour >  blank_end_h)   && (p.hour <  blank_begin_h)) ||
	    ((p.hour == blank_end_h)   && (p.min  >= blank_end_m))  ||
	    ((p.hour == blank_begin_h) && (p.min  <  blank_begin_m)))
	     return false;
	else return true;
} // blanking_active()


/*------------------------------------------------------------------------
  Purpose  : This task decide what to display on the Nixie Tubes.
			 Called once every second.
  Variables: nixie_bits (32 bits)
  Returns  : -
  ------------------------------------------------------------------------*/
void display_task(void)
{
	Time    p; // Time struct
	uint8_t x, y ;
	char    s2[40]; // Used for printing to RS232 port
	uint8_t	cnt_60sec   = 0;
	uint8_t blank_zero;
	
	nixie_bits  = 0x00000000; // clear all bits
	nixie_bits8 = 0x00;       // clear upper 8 bits
	ds3231_gettime(&p);
	display_time = false;     // start with no-time on display
	
	
	if (time_only == true)			// *0 IR remote
	{
		y = 0;
	}
	else
	{
		y = p.sec;
	}	//End time_only

	
	if (random_rgb_pattern == true)
	{
		fixed_random_rgb_colour(p.sec, true);
	}
	else if (fixed_rgb_pattern == true)
	{
		fixed_random_rgb_colour(p.sec, false);
	}
	else if (default_rgb_pattern == false)
	{
		switch (fixed_rgb_colour)
		{
			case 0:
			//rgb_colour = BLACK;
			ws2812b_fill_rgb(BLACK, BLACK, BLACK);
			break;
			case 1:
			//rgb_colour = RED;
			ws2812b_fill_rgb(RED, BLACK, BLACK);
			break;
			case 2:
			//rgb_colour = GREEN;
			ws2812b_fill_rgb(BLACK, GREEN, BLACK);
			break;
			case 3:
			//rgb_colour = BLUE;
			ws2812b_fill_rgb(BLACK, BLACK, BLUE);
			break;
			case 4:
			//rgb_colour = YELLOW;
			ws2812b_fill_rgb(RED, GREEN, BLACK);
			break;
			case 5:
			//rgb_colour = MAGENTA;
			ws2812b_fill_rgb(RED, BLACK, BLUE);
			break;
			case 6:
			//rgb_colour = CYAN;
			ws2812b_fill_rgb(BLACK, GREEN, BLUE);
			break;
			case 7:
			//rgb_colour = WHITE;
			ws2812b_fill_rgb(RED, GREEN, BLUE);
			break;
		} // End Switch
	} // End random_rgb_pattern
	
	
	if (test_nixies) ftest_nixies(); // S3 command
	else if (hv_relay_sw)
	{   // V0 or V1 command
		if (hv_relay_fx)
		     PORTB |=  HV_ON; // relay on
		else PORTB &= ~HV_ON; // relay off
	} // else if
	else if (blanking_active(p))
		PORTB &= ~HV_ON; // relay off
	else
	{
	  PORTB |=  HV_ON; // relay on	
	  switch(p.sec)
	  { 
		case 15: // display date & month
		case 16:
			check_and_set_summertime(p); // check for Summer/Wintertime change
			nixie_bits = encode_to_bcd(p.date);
			nixie_bits <<= 12;
			nixie_bits |= encode_to_bcd(p.mon);
			nixie_bits <<= 4;
			clear_nixie(3);
			clear_nixie(6);
			
			if (default_rgb_pattern == true)
			{
				//rgb_colour = GREEN;
				ws2812b_fill_rgb(BLACK, GREEN, BLACK);
			}			
		
		break;
		
		case 17: // display year
		case 18:
			PORTC &=~(0x0F);
			nixie_bits = encode_to_bcd(p.year / 100);
			nixie_bits <<= 8;
			nixie_bits |= encode_to_bcd(p.year % 100);
			nixie_bits <<= 4;
			clear_nixie(1);
			clear_nixie(6);
			
			if (default_rgb_pattern == true)
			{
				//rgb_colour = GREEN;
				ws2812b_fill_rgb(BLACK, GREEN, BLACK);
			}
			
		break;
		
		// case 30: // display humidity
		// case 31:
			// x = ((dht22_hum + Cal_Hum) / 10);	//Cal_Hum defined in Nixie.h to compensate DHT22 RH% mismatch
			// nixie_bits = encode_to_bcd(x);
			// nixie_bits <<= 4;
			// nixie_bits |= ((dht22_hum + Cal_Hum) - 10 * x);
			// nixie_bits |= RIGHT_DP5;
			// clear_nixie(1);
			// clear_nixie(2);
			// clear_nixie(3);
			
			// PORTC &=~(0x0F);
			// PORTC |= HUMIDITYSYMBOL;
			// PORTC |= LED_IN19A;
			
			// if (default_rgb_pattern == true)
			// {
				// rgb_colour = BLUE;
			// }
					
		// break;
		
		//case 37: // display dewpoint
		//case 38:
			//x = dht22_dewp / 10;
			//nixie_bits = encode_to_bcd(x);
			//nixie_bits <<= 4;
			//nixie_bits |= (dht22_dewp - 10 * x);
			//nixie_bits |= RIGHT_DP5;
			//clear_nixie(1);
			//clear_nixie(2);
			//clear_nixie(3);
			//
			//if (default_rgb_pattern == true	)
			//}
			//   rgb_colour = CYAN;
			//   PORTC &=~(0x0F);
			//   PORTC |= (HUMIDITYSYMBOL | LED_IN19A);
			//}
			//
		//break;			
		
		case 40: // display temperature
		case 41:
			
			x = (uint8_t)bmp180_temp;
			
			blank_zero = x;
			
			nixie_bits = encode_to_bcd(x);
			nixie_bits <<= 4;
			nixie_bits |= (uint8_t)(10.0 * (bmp180_temp - x));
			//nixie_bits <<= 12;
			
			//x = dht22_temp / 10;
			//nixie_bits = encode_to_bcd(x);
			//nixie_bits <<= 4;
			//nixie_bits |= (dht22_temp - 10 * x);
			nixie_bits |= LEFT_DP6;
			
			PORTC &=~(0x0F);
			PORTC |= DEGREESYMBOL;
			PORTC |= LED_IN19A;
			
			clear_nixie(1);
			clear_nixie(2);
			clear_nixie(3);

			xputs("Blank_zero Temp ");
			sprintf(s2,"%d\n",blank_zero);
			xputs(s2);
			
			if (blank_zero < 10)	// Blank 0 when temp < 10,0 degrees
			{
				clear_nixie(4);
			}			
			
			if (default_rgb_pattern == true)
			{
				//rgb_colour = RED;
				ws2812b_fill_rgb(RED, BLACK, BLACK);
			}
			
		break;

		case 50: // display Pressure in mbar
		case 51:
			x = (uint8_t)(bmp180_pressure / 100.0);
			
			blank_zero = x;
			
			nixie_bits = encode_to_bcd(x);
			nixie_bits <<= 8;
			x = (uint8_t)(bmp180_pressure - (int16_t)x * 100);
			nixie_bits |= encode_to_bcd(x);
			nixie_bits <<= 4;
			x = (uint8_t)(10 * (bmp180_pressure - (int16_t)bmp180_pressure));
			nixie_bits |= x;
			nixie_bits |= LEFT_DP6;

			PORTC &=~(0x0F);
			PORTC |= PRESSURESYMBOL;
			PORTC |= LED_IN19A;
			
			clear_nixie(1);
			
			xputs("Blank_zero Pressure ");
			sprintf(s2,"%d\n",blank_zero);
			xputs(s2);
			
			
			if (blank_zero < 10)	// Blank zero when pressure < 1000,0 mBar
			{
				clear_nixie(2);
			}
				
			if (default_rgb_pattern == true)
			{
				//rgb_colour = CYAN;
				ws2812b_fill_rgb(RED, BLACK, BLUE);
			}
			
		break;
	
		default: // display normal time
			display_time = true;
			check_and_set_summertime(p); // check for Summer/Wintertime change
			nixie_bits = encode_to_bcd(p.hour);
			nixie_bits <<= 8; // SHL 8
			nixie_bits |= encode_to_bcd(p.min);
			nixie_bits <<= 8; // SHL 8
			nixie_bits |= encode_to_bcd(p.sec);
			PORTC &=~(0x0F);
			if (default_rgb_pattern == true)
			{
				//rgb_colour = YELLOW;
				ws2812b_fill_rgb(RED, GREEN, BLACK);
			}
			// NOTE: LEFT_DP1..LEFT_DP4 are contained in upper 8 bits!
			if (p.sec & 0x01) nixie_bits  |=  RIGHT_DP4;
			else              nixie_bits  |=  LEFT_DP5;
			if (p.min & 0x01) nixie_bits8  =  LEFT_DP3;
			else              nixie_bits8  =  LEFT_DP3;
			if (dst_active)   nixie_bits8  |=  LEFT_DP1;
			else              nixie_bits8  &= ~LEFT_DP1;
			break;
	  } // switch
	} // else
} // display_task()


/*------------------------------------------------------------------------
Purpose  : Anti Nixie poisoning routine
Variables: -
Returns  : -
------------------------------------------------------------------------*/
//void Anti_Nixie_Poisoning(void)
//{
	//
	//uint8_t i = 0;
	//Time p;
	//
	//ds3231_gettime(&p);
	//
	//// Anti Nixie poisoning routine
	//// To avoid that Nixie numbers which are inactive are poisoned and light up darker or not anymore
	////if	(!((blanking_active(p) || (nixie_lifetimesaver == true)) && (display_60sec == false) && (override_lifetimesaver == false))) // To avoid anti poisoning during LST
	////{
		//if (((p.min == 43) || (p.min == 15) ||  (p.min == 30) || (p.min == 45)) && (p.sec == 0))
		//{
			//while(i < 10)
			//{
				//ftest_nixies();
				//++i;
			//}
			//i = 0;
		//}
	////}
//
//} // End Anti_Nixie_Poisoning


/*------------------------------------------------------------------------
Purpose  : Set the correct time in the DS3231 module via IRremote
Variables: -
Returns  : -
------------------------------------------------------------------------*/

void set_nixie_timedate(uint8_t x, uint8_t y, char z)
{
	switch (y)
	{
		case 0:
		nixie_bits |= x;
		clear_nixie(1);
		clear_nixie(2);
		clear_nixie(3);
		clear_nixie(4);
		clear_nixie(5);
		
				
		break;
		
		case 1:
		nixie_bits <<= 4;
		nixie_bits |= x;
		clear_nixie(1);
		clear_nixie(2);
		clear_nixie(3);
		clear_nixie(4);
				
		break;

		case 2:
		nixie_bits <<= 4;
		nixie_bits |= x;
		clear_nixie(1);
		clear_nixie(2);
		clear_nixie(3);
		
		break;
		
		case 3:
		nixie_bits <<= 4;
		nixie_bits |= x;
		clear_nixie(1);
		clear_nixie(2);		
		
		break;

		case 4:
		nixie_bits <<= 4;
		nixie_bits |= x;
		clear_nixie(1);		
		
		break;
		
		case 5:
		nixie_bits <<= 4;
		nixie_bits |= x;
				
		break;
		
		case 6:
		nixie_bits &= 0x00000000;
		PORTC &=~0x0F;
		
		break;
		
	} //switch

	//nixie_bits &= 0x00FFFFFF; // Clear decimal point bits 31 to 24
	nixie_bits8 &= 0x00; // Clear LEFT_DP1 t/m LEFT_DP4
	
	if (z == 'T')
	{
		nixie_bits  |= LEFT_DP5;
		nixie_bits8 |= LEFT_DP3;
		ws2812b_fill_rgb(RED,GREEN,BLACK); // YELLOW
		//rgb_colour = YELLOW;
	}
	else if (z == 'D')
	{
		nixie_bits  |= LEFT_DP5;
		nixie_bits8 |= LEFT_DP3;
		ws2812b_fill_rgb(BLACK, GREEN, BLACK); // GREEN
		//rgb_colour = GREEN;
	}
} // end set_nixie_timedate

/*------------------------------------------------------------------------
Purpose  : This function initializes Timer 2 for a 20 kHz (50 usec.)
           signal, because the IR library needs a 50 usec interrupt.
Variables: -
Returns  : -
------------------------------------------------------------------------*/
void init_timer2(void)
{
	// Set Timer 2 to 20 kHz interrupt frequency: ISR(TIMER2_COMPA_vect)
	TCCR2A |= (0x01 << WGM21); // CTC mode, clear counter on TCNT2 == OCR2A
	TCCR2B =  (0x01 << CS21);  // set pre-scaler to 8 (fclk = 2 MHz)
	OCR2A  =  99;    // this should set interrupt frequency to 20 kHz
	TCNT2  =  0;     // start counting at 0
	TIMSK2 |= (0x01 << OCIE2A);   // Set interrupt on Compare Match
} // init_timer2()

/*------------------------------------------------------------------------
Purpose  : This function initializes PORTB, PORTC and PORTD pins.
           See Nixie.h header for a detailed overview of all port-pins.
Variables: -
Returns  : -
------------------------------------------------------------------------*/
void init_ports(void)
{
	DDRB  &= ~(IR_RCV); // clear bits = input
	PORTB &= ~(IR_RCV); // disable pull-up resistors
	DDRB  |=  (TIME_MEAS | WS2812_DI | HV_ON); // set bits = output
	PORTB &= ~(TIME_MEAS | WS2812_DI | HV_ON); // init. outputs to 0
	DDRC |=(0x0F);   // set bits C3 t/m C0 as output
	PORTC &=~(0x0F); // set outputs to 0
	
	DDRD  |=  (SDIN | SHCP | STCP); // set bits = output
	PORTD &= ~(SDIN | SHCP | STCP); // set outputs to 0
	
} // init_ports()

/*------------------------------------------------------------------------
Purpose  : main() function: Program entry-point, contains all init. functions 
		   and calls the task-scheduler in a loop.
Variables: -
Returns  : -
------------------------------------------------------------------------*/
int main(void)
{
	
	uint8_t i = 0;
	Time    p; // Time struct
	char s[20];
	
	init_timer2(); // init. timer for scheduler and IR-receiver 
	ir_init();     // init. IR-library
	i2c_init(SCL_CLK_400KHZ); // Init. I2C HW with 400 kHz clock
	init_ports();  // init. PORTB, PORTC and PORTD port-pins 	
	
	srand(59);		// Initialize random generator from 0 - 59
	
	
	// Initialize Serial Communication, See usart.h for BAUD
	// F_CPU should be a Project Define (-DF_CPU=16000000UL)
	usart_init(MYUBRR); // Initializes the serial communication
	//bmp180_init();      // Init. the BMP180 sensor
	
	// Add tasks for task-scheduler here
	add_task(display_task ,"Display",  0, 1000); // What to display on the Nixies.
	add_task(update_nixies,"Update" ,100,   50); // Run Nixie Update every  50 msec.
	add_task(ir_receive   ,"IR_recv",150,  500); // Run IR-process   every 500 msec.
	//add_task(dht22_task   ,"DHT22"  ,250, 5000); // Run DHT22 sensor process every 5 sec.
	//add_task(bmp180_task  ,"BMP180" ,350, 1000); // Run BMP180 sensor process every second.
	
	sei(); // set global interrupt enable, start task-scheduler
	check_and_init_eeprom();  // Init. EEPROM
	read_eeprom_parameters();
	//dst_active = eeprom_read_byte(EEPARB_DST); // read from EEPROM
	xputs("Nixie board v0.30, Emile, Martijn, Ronald\n");
	xputs("Blanking from ");
	sprintf(s,"%02d:%02d to %02d:%02d\n",blank_begin_h,blank_begin_m,blank_end_h,blank_end_m);
	xputs(s);
	
	// Set WS2812b RGB Leds to black 
	ws2812b_fill_rgb(0x00, 0x00, 0x00);


	
	// Main routine 
	while(1)
	{   // Run all scheduled tasks here
		//ds3231_gettime(&p);
		dispatch_tasks(); // Run Task-Scheduler
		switch (rs232_command_handler()) // run command handler continuously
		{
			case ERR_CMD: xputs("Command Error\n");
			break;
			case ERR_NUM: sprintf(s,"Number Error (%s)\n",rs232_inbuf);
			xputs(s);
			break;
			default     : break;
		} // switch
	} // while
} // main()