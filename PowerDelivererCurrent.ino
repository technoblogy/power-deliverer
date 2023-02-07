/* Power Deliverer Current - see http://www.technoblogy.com/show?441V

   David Johnson-Davies - www.technoblogy.com - 7th February 2023
   ATtiny1604 @ 20 MHz (internal oscillator; BOD disabled)

   CC BY 4.0
   Licensed under a Creative Commons Attribution 4.0 International license: 
   http://creativecommons.org/licenses/by/4.0/
*/

#include <TinyI2CMaster.h>

// Bit field structures

// PD header
typedef union
{
  uint16_t d16;
  uint8_t bytes[2];
  struct
  {
    uint8_t messageType  : 5;  // Type of message: 1 = Source Capabilities
    uint8_t misc         : 4;
    uint8_t messageID    : 3;
    uint8_t objects      : 3;  // Number of PD objects received
    uint8_t reserved     : 1;
  };
} PDHeader_t;

// Power Data Object
typedef union
{
  uint8_t bytes[4];
  struct
  {
    uint32_t current    : 10;  // In units of 10mA
    uint32_t voltage    : 10;  // In units of 50mV
    uint32_t other      : 12;
  };
} PDO_t;

// OLED I2C 128 x 32 monochrome display **********************************************

const int OLEDAddress = 0x3C;
const int STUSBAddress = 0x28;

// Initialisation sequence for OLED module
int const InitLen = 14;
const unsigned char Init[InitLen] PROGMEM = {
  0xA8, // Set multiplex
  0x1F, // for 32 rows
  0x8D, // Charge pump
  0x14, 
  0x20, // Memory mode
  0x01, // Vertical addressing
  0xA1, // 0xA0/0xA1 flip horizontally
  0xC8, // 0xC0/0xC8 flip vertically
  0xDA, // Set comp ins
  0x02,
  0xD9, // Set pre charge
  0xF1,
  0xDB, // Set vcom deselect
  0x40,
};

const int data = 0x40;
const int single = 0x80;
const int command = 0x00;

void InitDisplay () {
  TinyI2C.start(OLEDAddress, 0);
  TinyI2C.write(command);
  for (uint8_t c=0; c<InitLen; c++) TinyI2C.write(pgm_read_byte(&Init[c]));
  TinyI2C.stop();
}

void DisplayOn () {
  TinyI2C.start(OLEDAddress, 0);
  TinyI2C.write(command);
  TinyI2C.write(0xAF);
  TinyI2C.stop();
}

// Display **********************************************

int Scale = 1; // 2 for big characters
enum chars { Space = 10, DP, Amps, Volts, Bra, Ket, CharN, CharP, CharD, Slash };

// 5x5 Character set for digits and a few characters - stored in program memory
const uint8_t CharMap[][6] PROGMEM = {
{ 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00 }, // '0'
{ 0x00, 0x12, 0x1F, 0x10, 0x00, 0x00 }, 
{ 0x12, 0x19, 0x15, 0x15, 0x12, 0x00 }, 
{ 0x09, 0x11, 0x15, 0x15, 0x0A, 0x00 }, 
{ 0x08, 0x0C, 0x0A, 0x1F, 0x08, 0x00 }, 
{ 0x17, 0x15, 0x15, 0x15, 0x09, 0x00 }, 
{ 0x0E, 0x15, 0x15, 0x15, 0x08, 0x00 }, 
{ 0x11, 0x09, 0x05, 0x03, 0x01, 0x00 }, 
{ 0x0A, 0x15, 0x15, 0x15, 0x0A, 0x00 }, 
{ 0x02, 0x15, 0x15, 0x15, 0x0E, 0x00 },
{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // Space
{ 0x00, 0x00, 0x18, 0x18, 0x00, 0x00 }, // DP
{ 0x1E, 0x05, 0x05, 0x05, 0x1E, 0x00 }, // A
{ 0x07, 0x08, 0x10, 0x08, 0x07, 0x00 }, // V
{ 0x00, 0x04, 0x0A, 0x11, 0x00, 0x00 }, // <
{ 0x00, 0x11, 0x0A, 0x04, 0x00, 0x00 }, // >
{ 0x1F, 0x02, 0x04, 0x08, 0x1F, 0x00 }, // N
{ 0x1F, 0x05, 0x05, 0x05, 0x02, 0x00 }, // P
{ 0x1F, 0x11, 0x11, 0x11, 0x0E, 0x00 }, // D
{ 0x10, 0x08, 0x04, 0x02, 0x01, 0x00 }, // Slash
};

// Converts bit pattern abcdefgh into aabbccddeeffgghh
int Stretch (int x) {
  x = (x & 0xF0)<<4 | (x & 0x0F);
  x = (x<<2 | x) & 0x3333;
  x = (x<<1 | x) & 0x5555;
  return x | x<<1;
}

// Buffer of 5 lines x 128 characters
uint8_t ScreenBuf[5][128];

// Clear the buffer
void ClearBuf () {
  for (uint8_t x=0; x<5; x++) for (uint8_t y=0; y<128; y++) ScreenBuf[x][y] = 0;
}

// Plots a character into buffer, with smoothing if Scale=2
void PlotChar (uint8_t c, uint8_t line, uint8_t column) {
  uint8_t col0 = pgm_read_byte(&CharMap[c][0]);
  int col0L, col0R, col1L, col1R;
  col0L = Stretch(col0); col0R = col0L;
  for (uint8_t col = 1; col < 5; col++) {
    uint8_t col1 = pgm_read_byte(&CharMap[c][col]);
    col1L = Stretch(col1); col1R = col1L;
    if (Scale == 1) ScreenBuf[line][column*6+col-1] = col0;
    // Smoothing
    else {
      for (int i=6; i>=0; i--) {
        for (int j=1; j<3; j++) {
          if (((col0>>i & 0b11) == (3-j)) && ((col1>>i & 0b11) == j)) {
            col0R = col0R | 1<<((i*2)+j);
            col1L = col1L | 1<<((i*2)+3-j);
          }
        }
      }
      ScreenBuf[line][column*6+col*2] = col0L;
      ScreenBuf[line+1][column*6+col*2] = col0L>>6;
      ScreenBuf[line][column*6+col*2+1] = col0R;
      ScreenBuf[line+1][column*6+col*2+1] = col0R>>6;
      col0L = col1L; col0R = col1R;
    }
  col0 = col1;
  }
  if (Scale == 1) ScreenBuf[line][column*6+5-1] = col0;
  else {
    ScreenBuf[line][column*6+5*2] = col0L;
    ScreenBuf[line+1][column*6+5*2] = col0L>>6;
    ScreenBuf[line][column*6+5*2+1] = col0R;
    ScreenBuf[line+1][column*6+5*2+1] = col0R>>6;
  }
}

void UpdateScreen () {
  TinyI2C.start(OLEDAddress, 0);
  TinyI2C.write(command);
  // Set column address range
  TinyI2C.write(0x21); TinyI2C.write(0); TinyI2C.write(127);
  // Set page address range
  TinyI2C.write(0x22); TinyI2C.write(0); TinyI2C.write(3);
  // Do it in 16 chunks because of buffer
  for (int c=0; c<32; c++) {
    TinyI2C.restart(OLEDAddress, 0);
    TinyI2C.write(data);
    for (int b=0; b<4; b++) {
      uint32_t bits = 0;
      for (int i=0 ; i<5; i++) {
        bits = bits<<6 | (ScreenBuf[4-i][c*4+b] & 0x3F);
      }
      for (int i=0; i<4; i++) {
        TinyI2C.write(bits & 0xFF);
        bits = bits>>8;
      }
    }
    TinyI2C.stop();
  }
}

// Display a current in mA or voltage in mV to 2 decimal places
void PlotVal (uint16_t value, int line, int column, boolean volts, boolean units) {
  boolean dig = false;
  if (volts) value = value * 5;
  for (long d = volts?1000:100; d; d=d/10) {
    char c = value/d % 10;
    if (c == 0 && !dig && d>100) c = Space; else dig = true;
    PlotChar(c, line, column);
    column = column + Scale;
    if (d == 100) {
      PlotChar(DP, line, column);
      column = column + Scale;
    }
  }
  if (units) {
    PlotChar(Space, line, column); column = column + 1;
    PlotChar(volts?Volts:Amps, line, column);
  }
}

// Plot an arrow cursor in columns 125 to 127
void PlotCursor (uint8_t line) {
  uint32_t col[3];
  col[0] = (uint32_t)0x04<<(6*line);
  col[1] = (uint32_t)0x0E<<(6*line);
  col[2] = (uint32_t)0x1F<<(6*line);
  TinyI2C.start(OLEDAddress, 0);
  TinyI2C.write(command);
  // Set column address range
  TinyI2C.write(0x21); TinyI2C.write(125); TinyI2C.write(127);
  // Set page address range
  TinyI2C.write(0x22); TinyI2C.write(0); TinyI2C.write(3);
  TinyI2C.restart(OLEDAddress, 0);
  TinyI2C.write(data);
  for (int c=0; c<3; c++) {
    for (int i=0; i<4; i++) {
      TinyI2C.write(col[c] & 0xFF);
      col[c] = col[c]>>8;
    }
  }
  TinyI2C.stop();
}

// Display the message "NO PD"
void DisplayNOPD () {
  Scale = 2;
  PlotChar(CharN, 2, 5); PlotChar(0, 2, 7);
  PlotChar(CharP, 2, 11); PlotChar(CharD, 2, 13);
  UpdateScreen();
  for(;;);
}

// Power Delivery **********************************************

// STUSB4500 registers we need
#define ALERT_STATUS      0x0B
#define ALERT_STATUS_MASK 0x0C
#define PORT_STATUS       0x0D
#define PRT_STATUS        0x16
#define PD_COMMAND_CTRL   0x1A
#define REG_DEVICE_ID     0x2F
#define TX_HEADER_LOW     0x51
#define DPM_PDO_NUMB      0x70
#define DPM_SNK_PDOS      0x85
#define RX_HEADER         0x31
#define RX_DATA_OBJS      0x33
#define RDO_REG_STATUS    0x91

// Constants
#define PRT_STATUS_AL          0x02
#define MSG_RECEIVED           0x04
#define SOURCE_CAPABILITIES    0x01

const int numObjects = 5;
PDO_t PDObject[numObjects];
PDHeader_t PDHeader;

volatile boolean GotPDOs = false;
volatile uint8_t Objects;

// Initialise the STUSB4500: read the status registers, and set an ALERT_STATUS_MASK
void InitSTUSB () {
  // Read status registers to clear flags
  TinyI2C.start(STUSBAddress, 0);
  TinyI2C.write(PORT_STATUS);
  TinyI2C.restart(STUSBAddress, 0x16-0x0D+1);
  for (int i=0; i<0x16-0x0D+1; i++) TinyI2C.read();

  // Only interested in PRT_STATUS_AL alert
  TinyI2C.restart(STUSBAddress, 0);
  TinyI2C.write(ALERT_STATUS_MASK);
  TinyI2C.write(~PRT_STATUS_AL);
  TinyI2C.stop();
}

// Read a single STUSB4500 register and return it
uint8_t ReadRegister (uint8_t reg) {
  TinyI2C.start(STUSBAddress, 0);
  TinyI2C.write(reg);
  TinyI2C.restart(STUSBAddress, 1);
  uint8_t val = TinyI2C.read();
  TinyI2C.stop();
  return val;
}

void SendSoftReset () {
  TinyI2C.start(STUSBAddress, 0);
  TinyI2C.write(TX_HEADER_LOW);
  TinyI2C.write(0x0D); // Soft reset
  TinyI2C.restart(STUSBAddress, 0);
  TinyI2C.write(PD_COMMAND_CTRL);
  TinyI2C.write(0x26); // Send command
  TinyI2C.stop();
}

// Read the PD header
void ReadHeader () {
  TinyI2C.start(STUSBAddress, 0);
  TinyI2C.write(RX_HEADER);
  TinyI2C.restart(STUSBAddress, 2);
  PDHeader.bytes[0] = TinyI2C.read(); PDHeader.bytes[1] = TinyI2C.read();
  TinyI2C.stop();
}

// Read the PD objects
void ReadObjects () {
  TinyI2C.start(STUSBAddress, 0);
  TinyI2C.write(RX_DATA_OBJS);
  TinyI2C.restart(STUSBAddress, numObjects*4);
  for (int obj=0; obj<numObjects; obj++) {
    for (int i=0; i<4; i++) PDObject[obj].bytes[i] = TinyI2C.read();
  }
  TinyI2C.stop();
}

// Copy selected source object to PDO 2
void SelectPDO (uint8_t object) {
  uint8_t index = 1;
  TinyI2C.start(STUSBAddress, 0);
  TinyI2C.write(DPM_SNK_PDOS + 4*index);
  for (int i=0; i<4; i++) TinyI2C.write(PDObject[object].bytes[i]);
  TinyI2C.stop();
}

void UpdatePDONumber (uint8_t Number_PDO) {
  if (Number_PDO >= 1 && Number_PDO <= 3) {
    TinyI2C.start(STUSBAddress, 0);
    TinyI2C.write(DPM_PDO_NUMB);
    TinyI2C.write(Number_PDO);
    TinyI2C.stop();
  }
}

void DisplayPDObjects () {
  for (int obj = 0; obj<Objects; obj++) {
    PlotChar(Bra, obj, 0);
    PlotChar(obj+1, obj, 1);
    PlotChar(Ket, obj, 2);
    PlotVal(PDObject[obj].voltage, obj, 5, true, true);
    PlotVal(PDObject[obj].current, obj, 13, false, true);
  }
}

// Alert interrupt **********************************************

void AlertEnable () {
  PORTB.PIN2CTRL = PORT_ISC_FALLING_gc;               // Pin change interrupt on PB2
}

void AlertDisable () {
  PORTB.PIN2CTRL = 0;
}

ISR(PORTB_PORT_vect) {
  PORTB.INTFLAGS = PIN2_bm;                           // Clear interrupt flag
  uint8_t alertStatus = ReadRegister(ALERT_STATUS);

  if ((alertStatus & PRT_STATUS_AL) != 0) {
    uint8_t prtStatus = ReadRegister(PRT_STATUS);
    if ((prtStatus & MSG_RECEIVED) != 0) {
      // Read message header
      ReadHeader();
      if (PDHeader.messageType == SOURCE_CAPABILITIES) {
        Objects = PDHeader.objects;
        ReadObjects();
        GotPDOs = true;
      }     
    }
  }
}

// Buttons **********************************************

const int Up = 5, Down = 6, Select = 4;
volatile boolean SelectPressed;
volatile uint8_t Cursor = 0;

void ButtonsOn () {
  // PA5 (Up), PA6 (Down), and PA4 (Select) inputs with pullups
  PORTA.PIN4CTRL = PORT_PULLUPEN_bm | PORT_ISC_FALLING_gc;
  PORTA.PIN5CTRL = PORT_PULLUPEN_bm | PORT_ISC_FALLING_gc;
  PORTA.PIN6CTRL = PORT_PULLUPEN_bm | PORT_ISC_FALLING_gc;;
}

void ButtonsOff () {
  PORTA.PIN5CTRL = PORT_PULLUPEN_bm;
  PORTA.PIN6CTRL = PORT_PULLUPEN_bm;
}

void WaitSelect () {
  SelectPressed = false;
  while(!SelectPressed);                               // Wait for Select
}

ISR(PORTA_PORT_vect) {
  uint8_t flags = PORTA.INTFLAGS;
  PORTA.INTFLAGS = 1<<Up | 1<<Down | 1<<Select;        // Clear interrupt flags
  if (flags & 1<<Select) { SelectPressed = true; return; }
  if (flags & 1<<Up) Cursor = (Cursor + Objects - 1) % Objects;
  if (flags & 1<<Down) Cursor = (Cursor + 1) % Objects;
  PlotCursor(Cursor);
}

// Main routines **********************************************

const int Sense = 3; // Arduino pin = PA7

void DisplayProfiles () {
  Scale = 1;
  UpdatePDONumber(1);                                 // Turn off output
  SendSoftReset();
  ClearBuf();
  DisplayPDObjects();
  UpdateScreen();
  PlotCursor(Cursor);
  ButtonsOn();
  WaitSelect();                                       // Wait for Select button
}

void DisplaySelected () {
  ButtonsOff();
  SelectPDO(Cursor);
  UpdatePDONumber(2);                                 // Turn on selected voltage
  SendSoftReset();
  ClearBuf();
  Scale = 2;
  PlotVal(PDObject[Cursor].voltage, 0, 8, true, true);
  PlotVal(PDObject[Cursor].current, 3, 10, false, true);
  PlotChar(Slash, 3, 8);
  SelectPressed = false;
  do {
    uint16_t current = (analogRead(Sense) * 29)/45;
    PlotVal(current, 3, 0, false, false);
    UpdateScreen();
    unsigned long now = millis();
    while (millis() - now < 1000);                    // Update once a second
  } while (!SelectPressed);                           // Wait for Select button
}

// Setup **********************************************

void setup() {
  delay(100);
  TinyI2C.init();
  InitDisplay();
  UpdateScreen();
  DisplayOn();
  
  // Wait for I2C ready
  while((ReadRegister(REG_DEVICE_ID) | 0x04) != 0x25);// ID is 0x21 or 0x25
  UpdatePDONumber(1);                                 // Choose 5V PDO only
  delay(500);
 
  // Initialise STUSB4500
  InitSTUSB();
  AlertEnable();
  
  // Send soft reset
  SendSoftReset();

  // Get RDO index to check USB input supports PD
  uint8_t rdoIndex = ReadRegister(RDO_REG_STATUS+3)>>4 & 0x07;
  if (rdoIndex == 0) DisplayNOPD();
  
  // Wait for PDs
  while(GotPDOs == false);
  AlertDisable();                                     // Don't need any more alerts
}

void loop() {
  DisplayProfiles();
  DisplaySelected();
}
