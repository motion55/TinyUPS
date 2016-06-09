/* Name: main.c
 * Project: TinyUPS
 * Author: Frank Zhao
 * Creation Date: 2016-06-08
 * Tabsize: 4
 * Copyright: (c) 2016 by Frank Zhao
 * License: GNU GPL v3 (see License.txt)
**/

/*
This is the main file of TinyUPS. It handles most of the USB communication and the scheduling of UPS tasks.
*/

#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include "usbdrv.h"

#include "ups.h"

#include "usbhiddesc.h"

report8_t reportBuffer8; // contains the battery status
report11_t reportBuffer11; // contains the charger status
report7_t reportBuffer7; // contains some parameter fields
report_debug_t reportBufferDebugOut, reportBufferDebugIn;
static report_byte_t reportBufferByte; // buffer space for reports that are a single byte

static uchar    idleRate;   /* repeat rate for keyboards, never used for mice */

/*
The descriptor includes some reports that are a single byte in length
in our minimal implementation, they are read-only
hence we can store them in a lookup table
If a dynamic implementation is required, this table serves as default values
*/
PROGMEM const uint8_t report_lookup_flash[] = {
    0,
    2,    // [ 1] string index for product
    3,    // [ 2] string index for serial number
    4,    // [ 3] string index for iDeviceChemistry
    5,    // [ 4] string index for iOEMInformation
    1,    // [ 5] Rechargable
    2,    // [ 6] CapacityMode, 0 = maH, 1 = mWH, 2 = %, 3 = boolean
    0,    // [ 7] taken care of
    0,    // [ 8] taken care of
    CONSTANT_INPUT_VOLTAGE, // [ 9] ConfigVoltage
    CONSTANT_INPUT_VOLTAGE, // [10] Voltage
    0,    // [11] taken care of
    0,    // [12] AudibleAlarmControl
    1,    // [13] string index for manufacture
    CONSTANT_INPUT_VOLTAGE, // [14] Input ConfigVoltage
    CONSTANT_INPUT_VOLTAGE, // [15] Input Voltage
    0,    // [16] Input LowVoltageTransfer
    0,    // [17] does not exist
    CONSTANT_OUTPUT_VOLTAGE, // [18] Output Voltage
    CONSTANT_OUTPUT_PERCENT_LOAD,   // [19] Output PercentLoad
    0,    // [20] Test
    0,    // [21] DelayBeforeShutdown
    0,    // [22] DelayBeforeStartup
    0,    // [23] Boost
    0,    // [24] ConfigActivePower
    0,    // [25] does not exist
    0,    // [26] ConfigApparentPower
};

#ifdef ALLOW_WRITE
uint8_t report_lookup[32];
#endif

uint8_t stdreq_buff[128];
static uint16_t currentPosition, bytesRemaining;
static char requestedHidDesc = 0;

PROGMEM const uint16_t usbDescriptorStringDeviceChemistry[] = {
    USB_STRING_DESCRIPTOR_HEADER(USB_CFG_DEVICE_CHEMISTRY_LEN),
    USB_CFG_DEVICE_CHEMISTRY
};

PROGMEM const uint16_t usbDescriptorStringOemInfo[] = {
    USB_STRING_DESCRIPTOR_HEADER(USB_CFG_OEM_INFO_LEN),
    USB_CFG_OEM_INFO
};

/*
V-USB by default does not handle HID descriptors that are longer than 255 bytes long
but we can tell it to delegate this under our control instead
this only works if USB_CFG_LONG_TRANSFERS is enabled
*/
USB_PUBLIC usbMsgLen_t usbFunctionDescriptor(struct usbRequest *rq) {
    if((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_STANDARD && rq->bRequest == USBRQ_GET_DESCRIPTOR && rq->wValue.bytes[1] == USBDESCR_HID_REPORT)
    {
        wdt_reset();
        usbMsgPtr = (usbMsgPtr_t)usbHidReportDescriptor;
        return (usbMsgLen_t)USB_CFG_HID_REPORT_DESCRIPTOR_LENGTH;
    }
    return usbFunctionSetup((uchar *)rq);
}

usbMsgLen_t usbFunctionSetup(uchar data[8])
{
    usbRequest_t* rq = (void *)data;

    requestedHidDesc = 0;

    if((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_STANDARD && rq->bRequest == USBRQ_GET_DESCRIPTOR && rq->wValue.bytes[1] == USBDESCR_STRING)
    {
        // there are additional strings that are described inside the HID report descriptor, requests will arrive to retrieve these additional strings
        usbMsgLen_t len;
        usbMsgPtr = (usbMsgPtr_t)stdreq_buff;
        switch (rq->wValue.bytes[0])
        {
            case 4: // iDeviceChemistry
                len = (usbMsgLen_t)sizeof(usbDescriptorStringDeviceChemistry);
                memcpy_P((void *)stdreq_buff, (const void *)usbDescriptorStringDeviceChemistry, (size_t)len);
                return len;
            case 5: // iOEMInformation
                len = (usbMsgLen_t)sizeof(usbDescriptorStringOemInfo);
                memcpy_P((void *)stdreq_buff, (const void *)usbDescriptorStringOemInfo, (size_t)len);
                return len;
        }
    }
    else if((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_STANDARD && rq->bRequest == USBRQ_GET_DESCRIPTOR && rq->wValue.bytes[1] == USBDESCR_HID_REPORT)
    {
        requestedHidDesc = 1;
        currentPosition = 0;                // initialize position index
        bytesRemaining = rq->wLength.word;  // store the amount of data requested
        if (bytesRemaining > USB_CFG_HID_REPORT_DESCRIPTOR_LENGTH) {
            bytesRemaining = USB_CFG_HID_REPORT_DESCRIPTOR_LENGTH;
        }
        return USB_NO_MSG; // calls usbFunctionWrite
    }
    else if((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_CLASS)
    {
        if(rq->bRequest == USBRQ_HID_GET_REPORT)
        {
            usbMsgLen_t ret = 0;
            // the computer seems to have a habit of requesting reports during enumeration, or during state switching events
            switch(rq->wValue.bytes[0]) { // check report ID
                case 8:
                    usbMsgPtr = (usbMsgPtr_t)&reportBuffer8;
                    ret = sizeof(report8_t);
                    break;
                case 11:
                    usbMsgPtr = (usbMsgPtr_t)&reportBuffer11;
                    ret = sizeof(report11_t);
                    break;
                case 7:
                    usbMsgPtr = (usbMsgPtr_t)&reportBuffer7;
                    ret = sizeof(report7_t);
                    break;
                case 0x20:
                    usbMsgPtr = (usbMsgPtr_t)&reportBufferDebugOut;
                    ret = sizeof(report_debug_t);
                    break;
                default:
                    reportBufferByte.report_id = rq->wValue.bytes[0];
                    if (rq->wValue.bytes[0] < 32 && rq->wLength.word <= 2) {
                        #ifdef ALLOW_WRITE
                        reportBufferByte.data = report_lookup[reportBufferByte.report_id];
                        #else
                        reportBufferByte.data = pgm_read_byte(&report_lookup_flash[reportBufferByte.report_id]);
                        #endif
                        usbMsgPtr = (usbMsgPtr_t)&reportBufferByte;
                        ret = sizeof(report_byte_t);
                    }
                    break;
            }
            if (ret > 0) {
                return ret;
            }
        }
        #ifdef ALLOW_WRITE
        else if(rq->bRequest == USBRQ_HID_SET_REPORT) {
            reportBufferByte.report_id = rq->wValue.bytes[0];
            currentPosition = 0;                // initialize position inde
            bytesRemaining = rq->wLength.word;  // store the amount of data requested
            if(bytesRemaining > sizeof(stdreq_buff)) // limit to buffer size
                bytesRemaining = sizeof(stdreq_buff);
            return USB_NO_MSG; // calls usbFunctionWrite
        }
        #endif
        else if(rq->bRequest == USBRQ_HID_GET_IDLE){
            usbMsgPtr = (usbMsgPtr_t)&idleRate;
            return 1;
        }
        else if(rq->bRequest == USBRQ_HID_SET_IDLE){
            idleRate = rq->wValue.bytes[1];
        }
    }else{
        /* no vendor specific requests implemented */
    }
    return 0;   /* default for not implemented requests: return no data back to host */
}

uint16_t usbFunctionRead(uchar *data, uint16_t len)
{
    uint16_t i;
    if(len > bytesRemaining)                // len is max chunk size
        len = bytesRemaining;               // send an incomplete chunk
    bytesRemaining -= len;
    if (requestedHidDesc != 0)
    {
        for(i = 0; i < len; i++) {
            data[i] = pgm_read_byte(&usbHidReportDescriptor[currentPosition]); // copy the data to the buffer
            currentPosition++;
        }
    }
    return len;                             // return real chunk size
}

#ifdef ALLOW_WRITE
uint8_t usbFunctionWrite(uint8_t *data, uint8_t len)
{
    uint8_t i;
    if(len > bytesRemaining)                // if this is the last incomplete chunk
        len = bytesRemaining;               // limit to the amount we can store
    bytesRemaining -= len;
    for(i = 0; i < len; i++)
        stdreq_buff[currentPosition++] = data[i];

    if (bytesRemaining == 0)
    {
        if (currentPosition == 1) {
            report_lookup[reportBufferByte.report_id] = stdreq_buff[0];
        }
        else if (reportBufferByte.report_id == 0x07) {
            memcpy(&reportBuffer7, stdreq_buff, currentPosition);
        }
        else if (reportBufferByte.report_id == 0x20) {
            memcpy(&reportBufferDebugIn, stdreq_buff, currentPosition);
        }
    }

    return bytesRemaining == 0;             // return 1 if we have all data
}
#endif

typedef uint64_t tmr_t;

int __attribute__((noreturn)) main(void)
{
    uchar i;

    tmr_t ms = 0;
    tmr_t tmrTx = 0;
    tmr_t tmrPollSts = 0, tmrPollBatt = 0;
    #if defined(USB_COUNT_SOF) && USB_COUNT_SOF != 0 && defined(USE_SOF_FOR_TIMING)
    tmr_t msOvf = 0;
    uint8_t prevSof = 0, curSof;
    #endif

    #ifdef USE_SOF_FOR_OSC_CAL
    // the ATtiny can calibrate its internal oscillator using the SOF events on the USB signals
    // the previous calibration value can be used as a starting point to speed up synchronization
    uchar   calibrationValue;
    calibrationValue = eeprom_read_byte(0); /* calibration value from last time */
    if(calibrationValue != 0xFF){
        OSCCAL = calibrationValue;
    }
    #endif

    #ifdef ALLOW_WRITE
    memcpy_P(report_lookup, report_lookup_flash, sizeof(report_lookup_flash));
    #endif

    ups_init();

    wdt_enable(WDTO_8S);
    wdt_reset();
    usbInit();
    usbDeviceDisconnect();
    _delay_ms(250); // fake disconnect to force re-enumeration
    usbDeviceConnect();
    sei();

    i = 0;
    for(;;)
    {
        //wdt_reset();
        // watchdog for this implementation only keeps the device alive if USB communication is active
        // see below, the wdt_reset is called when a message is sent to the host
        // this is good for a simple implementation, but undesirable for a more advanced implementation
        usbPoll();

        /*
        timing is not critical at all in this implementation
        if an implementation requires integration of current load to estimate battery life
        then you should be using a more capable chip than an ATtiny85
        which also means you'll use a crystal without requiring SOF calibration
        and then you can use an internal timer to keep time accurately
        */

        #if defined(USB_COUNT_SOF) && USB_COUNT_SOF != 0 && defined(USE_SOF_FOR_TIMING)
        curSof = usbSofCount; // warning, usbSofCount is volatile, best not to write to it ever
        if (prevSof > curSof) {
            msOvf += 256;
        }
        ms = msOvf + curSof;
        prevSof = curSof;
        #else
        _delay_us(999);
        ms++;
        #endif

        #ifdef ENABLE_UPS_REPORTS
        if(usbInterruptIsReady() && (ms - tmrTx) >= 500)
        {
            wdt_reset();
            if (i == 0)
            {
                reportBuffer11.report_id = 11;
                usbSetInterrupt((void *)&reportBuffer11, sizeof(report11_t));
            }
            else
            {
                reportBuffer8.report_id = 8;
                usbSetInterrupt((void *)&reportBuffer8, sizeof(report8_t));
            }
            tmrTx = ms;
            i ^= 1;
        }
        #endif
        if ((ms - tmrPollBatt) >= 10)
        {
            poll_batt();
            tmrPollBatt = ms;
        }
        if ((ms - tmrPollSts) >= 250)
        {
            poll_status();
            tmrPollSts = ms;
        }
        report_fill();
    }
}

#ifdef USE_SOF_FOR_OSC_CAL
// use the frame length to calibrate the internal oscillator
// http://vusb.wikidot.com/examples
static void calibrateOscillator(void)
{
    uchar       step = 128;
    uchar       trialValue = 0, optimumValue;
    int         x, optimumDev, targetValue = (unsigned)(1499 * (double)F_CPU / 10.5e6 + 0.5);

    /* do a binary search: */
    do{
        OSCCAL = trialValue + step;
        x = usbMeasureFrameLength();    /* proportional to current real frequency */
        if(x < targetValue)             /* frequency still too low */
            trialValue += step;
        step >>= 1;
    } while(step > 0);
    /* We have a precision of +/- 1 for optimum OSCCAL here */
    /* now do a neighborhood search for optimum value */
    optimumValue = trialValue;
    optimumDev = x; /* this is certainly far away from optimum */
    for(OSCCAL = trialValue - 1; OSCCAL <= trialValue + 1; OSCCAL++){
        x = usbMeasureFrameLength() - targetValue;
        if(x < 0)
            x = -x;
        if(x < optimumDev){
            optimumDev = x;
            optimumValue = OSCCAL;
        }
    }
    OSCCAL = optimumValue;
}

void usbEventResetReady(void)
{
    /* Disable interrupts during oscillator calibration since
     * usbMeasureFrameLength() counts CPU cycles.
     */
    cli();
    calibrateOscillator();
    sei();
    eeprom_write_byte(0, OSCCAL);   /* store the calibrated value in EEPROM */
}

#endif
