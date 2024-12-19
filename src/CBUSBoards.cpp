
#include "CBUS.h"

/// IO pin set class

BoardIOPinSet::BoardIOPinSet() {

}

BoardIOPinSet::BoardIOPinSet(byte *_pins) {

	setPins(_pins);
}

void BoardIOPinSet::setPins(byte *_pins) {

	for (byte i = 0; i < 8; i++) {
		pin_array[i] = _pins[i];
	}
}

byte BoardIOPinSet::pin(byte pin_number) {

	return (pin_number < 8) ? pin_array[pin_number] : 255;
}

/// base class

MainBoardBase::MainBoardBase() {

}

MainBoardBase::~MainBoardBase() {

}

/// board hardware classes

Pico_Mainboard_rev_C::Pico_Mainboard_rev_C() {

	upper.setPins(upper_pins);
	lower.setPins(lower_pins);
}

//

MegaAVR_mainboard_rev_C::MegaAVR_mainboard_rev_C() {

	upper.setPins(upper_pins);
	lower.setPins(lower_pins);
}

//

ESP32_mainboard_rev_C::ESP32_mainboard_rev_C() {

	upper.setPins(upper_pins);
	lower.setPins(lower_pins);
}

//

Nano_mainboard_rev_C::Nano_mainboard_rev_C() {

	upper.setPins(upper_pins);
	lower.setPins(lower_pins);
}

//

AVRDA_mainboard_rev_C::AVRDA_mainboard_rev_C() {

	upper.setPins(upper_pins);
	lower.setPins(lower_pins);
}

//

