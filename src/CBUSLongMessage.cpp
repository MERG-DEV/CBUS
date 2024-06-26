
/*

  Copyright (C) Duncan Greenwood 2017 (duncan_greenwood@hotmail.com)
  Based on MERGG RFC 0005 by Dave McCabe (M4933)

  This work is licensed under the:
      Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License.
   To view a copy of this license, visit:
      http://creativecommons.org/licenses/by-nc-sa/4.0/
   or send a letter to Creative Commons, PO Box 1866, Mountain View, CA 94042, USA.

   License summary:
    You are free to:
      Share, copy and redistribute the material in any medium or format
      Adapt, remix, transform, and build upon the material

    The licensor cannot revoke these freedoms as long as you follow the license terms.

    Attribution : You must give appropriate credit, provide a link to the license,
                  and indicate if changes were made. You may do so in any reasonable manner,
                  but not in any way that suggests the licensor endorses you or your use.

    NonCommercial : You may not use the material for commercial purposes. **(see note below)

    ShareAlike : If you remix, transform, or build upon the material, you must distribute
                 your contributions under the same license as the original.

    No additional restrictions : You may not apply legal terms or technological measures that
                                 legally restrict others from doing anything the license permits.

   ** For commercial use, please contact the original copyright holder(s) to agree licensing terms

    This software is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE

*/

//
/// Implementation of CBUS Long Message Support RFC 0005 by Dave McCabe (M4933)
/// Developed by Duncan Greenwood (M5767)
//

#include <CBUS.h>
#include <Streaming.h>

uint16_t crc16(uint8_t *data_p, uint16_t length);
uint32_t crc32(const char *s, size_t n);

//
/// log a message with a variable number of arguments
//

#define VLOG_DEBUG 0
#define DEBUG_MSG_LEN 256

/*
void VLOG(const char fmt[], ...) {

#if VLOG_DEBUG == 0
	return;
#endif

	va_list vptr;
	char tmpbuff[DEBUG_MSG_LEN];

	if (strlen(fmt) == 0) {
		Serial.printf("\n");
	} else {
		va_start(vptr, fmt);
		vsnprintf(tmpbuff, DEBUG_MSG_LEN - 1, fmt, vptr);
		va_end(vptr);
		Serial.printf("%10lu: %s\n", millis(), tmpbuff);
	}

	return;
}

*/

//
/// constructor
/// receives a pointer to a CBUS object which provides the CAN message handling capability
//

CBUSLongMessage::CBUSLongMessage(CBUSbase *cbus_object_ptr) {

	_cbus_object_ptr = cbus_object_ptr;
	_cbus_object_ptr->setLongMessageHandler(this);
}

//
/// subscribe to a range of stream IDs
//

void CBUSLongMessage::subscribe(byte *stream_ids, const byte num_stream_ids, void *receive_buffer, const unsigned int receive_buff_len, void (*messagehandler)(void *msg, unsigned int msg_len, byte stream_id, byte status)) {

	_stream_ids = stream_ids;
	_num_stream_ids = num_stream_ids;
	_receive_buffer = (byte *)receive_buffer;
	_receive_buffer_len = receive_buff_len;
	_messagehandler = messagehandler;

	// DEBUG_SERIAL << F("> subscribe: num_stream_ids = ") << num_stream_ids << F(", receive_buff_len = ") << receive_buff_len << endl;
	return;
}

//
/// initiate sending of a long message
/// this method sends the first message - the header fragment
/// the remainder of the message is sent in chunks from the process() method
//

bool CBUSLongMessage::sendLongMessage(const void *msg, const unsigned int msg_len, const byte stream_id, const byte priority) {

	CANFrame frame;

	// DEBUG_SERIAL << F("> L: sending message header fragment, stream id = ") << stream_id << F(", message length = ") << msg_len << F(", first char = ") << (char)msg[0] << endl;

	if (is_sending()) {
		// DEBUG_SERIAL << F("> L: ERROR: there is already a message in progress") << endl;
		return false;
	}

	// initialise variables
	_send_buffer = (byte *)msg;
	_send_buffer_len = msg_len;
	_send_stream_id = stream_id;
	_send_priority = priority;
	_send_buffer_index = 0;
	_send_sequence_num = 0;

	// send the first fragment which forms the message header
	frame.data[1] = _send_stream_id;																									// the unique stream id
	frame.data[2] = _send_sequence_num;																								// sequence number, 0 = header fragment
	frame.data[3] = highByte(msg_len);																								// the message length
	frame.data[4] = lowByte(msg_len);
	frame.data[5] = 0;																																// CRC - not implemented for lite version
	frame.data[6] = 0;
	frame.data[7] = 0;																																// flags - 0 = standard data message

	bool ret = sendMessageFragment(&frame, _send_priority);														// send the header fragment
	++_send_sequence_num;																															// increment the sending sequence number - it's fine if it wraps around

	// DEBUG_SERIAL << F("> L: message header sent, stream id = ") << _send_stream_id << F(", message length = ") << _send_buffer_len << endl;
	return (ret);
}

//
/// the process method is called regularly from the user's loop function
/// we use this to send the individual fragments of an outgoing message and check the message receive timeout
//

bool CBUSLongMessage::process(void) {

	bool ret = true;
	byte i;
	CANFrame frame;

	/// check receive timeout

	if (_is_receiving && (millis() - _last_fragment_received >= _receive_timeout)) {
		// DEBUG_SERIAL << F("> L: ERROR: timed out waiting for continuation fragment") << endl;
		(void)(*_messagehandler)(_receive_buffer, _receive_buffer_index, _receive_stream_id, CBUS_LONG_MESSAGE_TIMEOUT_ERROR);
		_is_receiving = false;
		_incoming_message_length = 0;
		_incoming_bytes_received = 0;

		// timeout error status is surfaced to the user's handler function
	}

	/// send the next outgoing fragment, after a configurable delay to avoid flooding the bus

	if (_send_buffer_index < _send_buffer_len && (millis() - _last_fragment_sent >= _msg_delay)) {

		_last_fragment_sent = millis();

		memset(&frame.data, 0, sizeof(frame.data));
		frame.data[1] = _send_stream_id;
		frame.data[2] = _send_sequence_num;

		/// only the last fragment is potentially less than 5 bytes long

		for (i = 0; i < 5 && _send_buffer_index < _send_buffer_len; i++) {								// for up to 5 bytes of payload
			frame.data[i + 3] = _send_buffer[_send_buffer_index];														// take the next byte
			// DEBUG_SERIAL << F("> L: consumed data byte = ") << (char)_send_buffer[_send_buffer_index] << endl;
			++_send_buffer_index;
		}

		ret = sendMessageFragment(&frame, _send_priority);																			// send the data fragment
		// DEBUG_SERIAL << F("> L: process: sent message fragment, seq = ") << _send_sequence_num << F(", size = ") << i << endl;

		++_send_sequence_num;
	}

	return ret;
}

//
/// handle a received long message fragment
/// this method is called by the main CBUS object each time a long CBUS message is received (opcode 0xe9)
//

void CBUSLongMessage::processReceivedMessageFragment(const CANFrame *frame) {

	/// handle a received message fragment

	// DEBUG_SERIAL << F("> L: processing received long message fragment, message length = ") << _incoming_message_length << F(", rcvd so far = ") << _incoming_bytes_received << endl;

	_last_fragment_received = millis();

	byte i, j;

	if (!_is_receiving) {																																	// not currently receiving a long message

		if (frame->data[2] == 0) {																													// sequence zero = a header fragment with start of new stream
			if (frame->data[7] == 0) {																												// flags = 0, standard messages
				for (i = 0; i < _num_stream_ids; i++) {
					if (_stream_ids[i] == frame->data[1]) {																				// are we subscribed to this stream id ?
						_is_receiving = true;
						_receive_stream_id = frame->data[1];
						_incoming_message_length = (frame->data[3] << 8) + frame->data[4];
						_incoming_message_crc = (frame->data[5] << 8) + frame->data[6];
						_incoming_bytes_received = 0;
						memset(_receive_buffer, 0, _receive_buffer_len);
						_receive_buffer_index = 0;
						_expected_next_receive_sequence_num = 0;
						_sender_canid = (frame->id & 0x7f);
						// DEBUG_SERIAL << F("> L: received header fragment for stream id = ") << _receive_stream_id << F(", message length = ") << _incoming_message_length << F(", user buffer len = ") << _receive_buffer_len << endl;
						break;
					}
				}
			} else {
				// DEBUG_SERIAL << F("> L: not handling message with non-zero flags") << endl;
			}
		}

	} else {																																							// we're part way through receiving a message

		if ((frame->id & 0x7f) == _sender_canid) {																					// it's the same sender CANID

			if (frame->data[1] == _receive_stream_id) {																			  // it's the same stream id

				if (frame->data[2] == _expected_next_receive_sequence_num) {										// and it's the expected sequence id

					// DEBUG_SERIAL << F("> L: received continuation fragment, seq = ") << _expected_next_receive_sequence_num << endl;

					// for each of the maximum five payload bytes, up to the total message length and the user buffer length
					for (j = 0; j < 5; j++) {

						_receive_buffer[_receive_buffer_index] = frame->data[j + 3];								// take the next byte
						++_receive_buffer_index;																										// increment the buffer index
						++_incoming_bytes_received;
						// DEBUG_SERIAL << F("> L: processing received data byte = ") << (char)frame->data[j + 3] << endl;

						// if we have read the entire message
						if (_incoming_bytes_received >= _incoming_message_length) {
							// DEBUG_SERIAL << F("> L: bytes processed = ") << _incoming_bytes_received << F(", message data has been fully consumed") << endl;
							(void)(*_messagehandler)(_receive_buffer, _receive_buffer_index, _receive_stream_id, CBUS_LONG_MESSAGE_COMPLETE);
							_receive_buffer_index = 0;
							memset(_receive_buffer, 0, _receive_buffer_len);
							break;

							// if the user buffer is full, give the user what we have so far
						} else if (_receive_buffer_index >= _receive_buffer_len ) {
							// DEBUG_SERIAL << F("> L: user buffer is full") << endl;
							(void)(*_messagehandler)(_receive_buffer, _receive_buffer_index, _receive_stream_id, CBUS_LONG_MESSAGE_INCOMPLETE);
							_receive_buffer_index = 0;
							memset(_receive_buffer, 0, _receive_buffer_len);
						}
					}

				} else {																																				// it's the wrong sequence id
					// DEBUG_SERIAL << F("> L: ERROR: expected receive sequence num = ") << _expected_next_receive_sequence_num << F(" but got = ") << frame->data[2] << endl;
					(void)(*_messagehandler)(_receive_buffer, _receive_buffer_index, _receive_stream_id, CBUS_LONG_MESSAGE_SEQUENCE_ERROR);
					_incoming_message_length = 0;
					_incoming_bytes_received = 0;
					_is_receiving = false;
				}

			} else {																																					// probably another stream in progress - we'll ignore it as we don't support concurrent streams
				// DEBUG_SERIAL << F("> L: ignoring unexpected stream id =") << _receive_stream_id << F(", got = ") << frame->data[2] << endl;
			}

		} else {																																						// a different sender CANID - ignore the fragment
			// DEBUG_SERIAL << F("> L: ignoring fragment from unexpected CANID") << endl;
		}

	}	// it's a continuation fragment

	// the sequence number may wrap from 255 to 0, which is absolutely fine
	++_expected_next_receive_sequence_num;

	/// finally, once the message has been completely received ...

	if (_incoming_message_length > 0 && _incoming_bytes_received >= _incoming_message_length) {
		// DEBUG_SERIAL << F("> L: message is complete") << endl;

		// surface any final fragment to the user's code
		if (_receive_buffer_index > 0) {
			// DEBUG_SERIAL << F("> L: surfacing final fragment") << endl;
			(void)(*_messagehandler)(_receive_buffer, _receive_buffer_index, _receive_stream_id, CBUS_LONG_MESSAGE_COMPLETE);
		}

		// get ready for the next long message
		_incoming_message_length = 0;
		_incoming_bytes_received = 0;
		_receive_buffer_index = 0;
		_is_receiving = false;
	}

	return;
}

//
/// report progress of sending last long message
/// true = complete, false = in progress, incomplete
/// user code must not start a new message until the previous one has been completely sent
//

bool CBUSLongMessage::is_sending(void) {

	return (_send_buffer_index < _send_buffer_len);
}

//
/// send next message fragment
//

bool CBUSLongMessage::sendMessageFragment(CANFrame *frame, const byte priority) {

	bool ret;
	char buffer[64], t[8];

	// these are common to all messages
	frame->len = 8;
	frame->data[0] = OPC_DTXC;

	ret = (_cbus_object_ptr->sendMessage(frame, false, false, priority));

	sprintf(buffer, "[%lu] [%u] [ ", frame->id, frame->len);
	for (byte i = 0; i < frame->len; i++) {
		sprintf(t, "%02x ", frame->data[i]);
		strcat(buffer, t);
	}
	sprintf(t, "]");
	strcat(buffer, t);

	// VLOG(buffer);

	return ret;
}

//
/// set the delay between send fragments, to avoid flooding the bus and other modules
/// overrides the default value
//

void CBUSLongMessage::setDelay(byte delay_in_millis) {

	_msg_delay = delay_in_millis;
	return;
}

//
/// set the receive timeout
/// if an expected next fragment is not received, the user's handler function
/// will be called with the message so far assembled and an error status
/// overrides the default value
//

void CBUSLongMessage::setTimeout(unsigned int timeout_in_millis) {

	_receive_timeout = timeout_in_millis;
	return;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

//
//// extended support for multiple concurrent long messages
//

//
/// allocate memory for receive and send contexts
//

bool CBUSLongMessageEx::allocateContexts(byte num_receive_contexts, unsigned int receive_buffer_len, byte num_send_contexts) {

	return allocateContextsBuffers(num_receive_contexts, receive_buffer_len, num_send_contexts, 0);
}

bool CBUSLongMessageEx::allocateContextsBuffers(byte num_receive_contexts, unsigned int receive_buffer_len, byte num_send_contexts, unsigned int send_buffer_len) {


	byte i;

	_num_receive_contexts = num_receive_contexts;
	_receive_buffer_len = receive_buffer_len;
	_num_send_contexts = num_send_contexts;
	_send_buffer_len = send_buffer_len;

	// allocate receive contexts
	if ((_receive_contexts = (receive_context_t **)malloc(sizeof(receive_context_t *) * _num_receive_contexts)) == NULL) {
		return false;
	}

	for (i = 0; i < _num_receive_contexts; i++) {
		if ((_receive_contexts[i] = (receive_context_t *)malloc(sizeof(receive_context_t))) == NULL) {
			return false;
		}

		if ((_receive_contexts[i]->buffer = (byte *)malloc(receive_buffer_len * sizeof(byte))) == NULL) {
			return false;
		}

		_receive_contexts[i]->in_use = false;
	}

	// allocate send contexts - user code provides the buffer when sending
	if ((_send_contexts = (send_context_t **)malloc(sizeof(send_context_t *) * _num_send_contexts)) == NULL) {
		return false;
	}

	for (i = 0; i < _num_send_contexts; i++) {
		if ((_send_contexts[i] = (send_context_t *)malloc(sizeof(send_context_t))) == NULL) {
			return false;
		}

		if (send_buffer_len > 0) {
			if ((_send_contexts[i]->buffer = (byte *)malloc(send_buffer_len * sizeof(byte))) == NULL) {
				return false;
			}
		}

		_send_contexts[i]->in_use = false;
		_send_contexts[i]->is_current = false;
	}

	current_send_context = 0;
	_last_fragment_sent = millis();			// in any context

	// VLOG("allocated send and receive contexts ok");
	return true;
}

//
/// initiate sending of a long message
/// this method sends the first message - the header fragment
/// the remainder of the message is sent in fragments from the process() method
//

bool CBUSLongMessageEx::sendLongMessage(const void *msg, const unsigned int msg_len, const byte stream_id, const byte priority) {

	byte context, j;
	CANFrame frame;

	// VLOG("submitting message, stream = %u, len - %u", stream_id, msg_len);

	// if interleaving, ensure we aren't already sending a message with this stream ID
	if (!_is_sequential) {
		for (context = 0; context < _num_send_contexts; context++) {
			if (_send_contexts[context]->in_use && _send_contexts[context]->send_stream_id == stream_id) {
				// VLOG("ERROR: already sending on stream = %u", stream_id);
				return false;
			}
		}
	}

	// find a free send context
	for (context = 0; context < _num_send_contexts; context++) {
		if (!_send_contexts[context]->in_use) {
			break;
		}
	}

	// no free context avalable
	if (context >= _num_send_contexts) {
		// VLOG("ERROR: unable to find a free send context");
		return false;
	}

	// VLOG("using send context = %u", context);

	// initialise context
	_send_contexts[context]->in_use = true;

	// for sequential transmissions, determine if this new message is the current context
	// we are current by default if no other contexts are current
	// if we are not current, we will wait until the current message has been completely sent

	if (_is_sequential) {
		_send_contexts[context]->is_current = true;											// this message is current by default, unless ...

		// is another context current ?
		for (j = 0; j < _num_send_contexts; j++) {
			if (j != context && _send_contexts[j]->is_current) {						// another context is current
				_send_contexts[context]->is_current = false;									// so this message is not
				break;
			}
		}

		if (_send_contexts[context]->is_current) {
			current_send_context = context;																	// this is the only active context so make it the next to run
			// VLOG("this message is current and will transmit now");
		} else {
			// VLOG("this message is not current and will wait");
		}
	}

	if (!_is_sequential) {
		byte num_in_use = 0;

		for (j = 0; j < _num_send_contexts; j++) {
			if (_send_contexts[j]->in_use) {
				++num_in_use;
			}
		}

		if (num_in_use == 1) {
			current_send_context = context;																	// this is the only in-use context so make it the next to run
			// VLOG("this is the only current non-sequential context in use");
		}
	}

	if (_send_buffer_len > 0) {																					// if this context already has buffer space allocated
		byte *ptr = (byte *) msg;
		byte len = msg_len;

		if (len > _send_buffer_len) {
			len = _send_buffer_len;
		}

		// VLOG("using existing send buffer space, size = %u, msg len = %u", _send_buffer_len, len);
		memcpy(_send_contexts[context]->buffer, ptr, len);								// copy in the message data

	} else {
		// VLOG("duplicating message content - must free later");
		_send_contexts[context]->buffer = (byte *)strdup((char *)msg);		// make a copy the message in the send context, will free later
	}

	_send_contexts[context]->send_stream_id = stream_id;																			// stream ID
	_send_contexts[context]->send_priority = priority;																				// CAN send priority
	_send_contexts[context]->send_buffer_len = msg_len;																				// message length
	_send_contexts[context]->send_buffer_index = 0;																						// current offset into data buffer
	_send_contexts[context]->send_time = micros();																						// when this message was submitted, to ensure queued messages are sent in order
	_send_contexts[context]->msg_crc = _use_crc ? crc16((uint8_t *)msg, msg_len) : 0;					// CRC
	_send_contexts[context]->send_sequence_num = 0;																						// next fragmant to send is the header
	_send_contexts[context]->last_fragment_sent = millis();																		// seed this value so message does not transmit immediately

	// VLOG("message queued for transmission");
	// VLOG("");

	return true;
}

//
/// the process method is called regularly from the application loop function
/// we use this to check for message receive timeouts and to send the individual fragments of any outgoing messages
//

bool CBUSLongMessageEx::process(void) {

	bool ret = true;
	byte i, j;
	CANFrame frame;

	/// check receive timeout for each active context

	for (i = 0; i < _num_receive_contexts; i++) {
		if (_receive_contexts[i]->in_use && (millis() - _receive_contexts[i]->last_fragment_received >= _receive_timeout)) {
			// VLOG("ERROR: tiemed out waiting for continuation fragment in context = %u, timeout = %u", i, _receive_timeout);
			(void)(*_messagehandler)(_receive_contexts[i]->buffer, _receive_contexts[i]->receive_buffer_index, _receive_contexts[i]->receive_stream_id, CBUS_LONG_MESSAGE_TIMEOUT_ERROR);
			_receive_contexts[i]->in_use = false;
		}
	}

	/// select the next context to process

	/// if sequential, process the in-progress context until it has been completely sent, then activate the next context, if any

	if (_is_sequential) {
		for (j = 0; j < _num_send_contexts; j++) {
			if (_send_contexts[j]->in_use && _send_contexts[j]->is_current) {
				current_send_context = j;
				break;
			}
		}
	}

	/// process and send the next fragment of the selected context, after a configurable delay to avoid flooding the bus

	if (_send_contexts[current_send_context]->in_use && (millis() - _send_contexts[current_send_context]->last_fragment_sent > _msg_delay) && (millis() - _last_fragment_sent > _msg_delay)) {

		// VLOG("");
		// VLOG("processing send context = %u, seq = %u, mode = %c", current_send_context, _send_contexts[current_send_context]->send_sequence_num, (_is_sequential ? 'S' : 'I'));

		memset(&frame.data, 0, sizeof(frame.data));																											// clear the CAN message
		frame.data[1] = _send_contexts[current_send_context]->send_stream_id;														// the stream id
		frame.data[2] = _send_contexts[current_send_context]->send_sequence_num;												// sequence number

		if (_send_contexts[current_send_context]->send_sequence_num == 0) {															// it's the header fragment

			// VLOG("sending header fragment for stream = %u", _send_contexts[current_send_context]->send_stream_id);

			// send the first fragment which forms the message header containing metadata
			frame.data[3] = highByte(_send_contexts[current_send_context]->send_buffer_len);						  // the message length
			frame.data[4] = lowByte(_send_contexts[current_send_context]->send_buffer_len);
			frame.data[5] = highByte(_send_contexts[current_send_context]->msg_crc);										  // CRC, or zero if not implemented
			frame.data[6] = lowByte(_send_contexts[current_send_context]->msg_crc);
			frame.data[7] = 0;																																						// flags - 0 = standard data message

			_send_contexts[current_send_context]->last_fragment_sent = millis();													// timestamp when this fragment was sent

		} else {																																												// it's a continuation fragment

			// VLOG("sending continuation fragment for stream = %u", _send_contexts[current_send_context]->send_stream_id );

			// only the final fragment is potentially less than 5 bytes long
			for (i = 0; i < 5 && _send_contexts[current_send_context]->send_buffer_index < _send_contexts[current_send_context]->send_buffer_len; i++) {								// for up to 5 bytes of payload
				frame.data[i + 3] = _send_contexts[current_send_context]->buffer[_send_contexts[current_send_context]->send_buffer_index];																// take the next byte
				// VLOG("consumed data byte = %c", (char)_send_contexts[current_send_context]->buffer[_send_contexts[current_send_context]->send_buffer_index]);
				++_send_contexts[current_send_context]->send_buffer_index;
			}

			// VLOG("consumed %u data bytes for this fragment", i);
		}

		/// send the message fragment

		ret = sendMessageFragment(&frame, _send_contexts[current_send_context]->send_priority);						// send the fragment
		// VLOG("sent message fragment, seq = %u, ret = %u", _send_contexts[current_send_context]->send_sequence_num, ret);

		/// release the context once message content is exhausted

		if (_send_contexts[current_send_context]->send_buffer_index >= _send_contexts[current_send_context]->send_buffer_len) {

			// VLOG("clearing completed context = %u", current_send_context);

			_send_contexts[current_send_context]->in_use = false;
			_send_contexts[current_send_context]->is_current = false;
			_send_contexts[current_send_context]->send_buffer_len = 0;

			if (_send_buffer_len == 0) {
				// VLOG("freeing buffer");
				free(_send_contexts[current_send_context]->buffer);
			}

			/// if sending sequentially, find the oldest active context and make it current

			if (_is_sequential) {
				bool found_next_context = false;
				byte next_context_idx = 0;
				unsigned long oldest_time = 0xffffffff;

				// VLOG("looking for next sequential context to activate");

				for (j = 0; j < _num_send_contexts; j++) {
					if (_send_contexts[j]->in_use && _send_contexts[j]->send_time < oldest_time) {
						found_next_context = true;
						next_context_idx = j;
						oldest_time = _send_contexts[j]->send_time;
						break;
					}
				}

				if (found_next_context) {
					_send_contexts[next_context_idx]->is_current = true;
					// VLOG("next sequential context = %u", next_context_idx);
				} else {
					// VLOG("no context to activate");
				}
			}

			// VLOG("** message sending complete, context released");

		} else {

			// sending is not complete, increment counters
			++_send_contexts[current_send_context]->send_sequence_num;
			_send_contexts[current_send_context]->last_fragment_sent = millis();				// this context
			// VLOG("next sequence number for this context = %u", _send_contexts[current_send_context]->send_sequence_num);
		}

		_last_fragment_sent = millis();																								// any context

		// if interleaving, locate the next context to process by round-robin
		if (!_is_sequential) {
			for (j = 0; j < _num_send_contexts; j++) {
				current_send_context = (current_send_context + 1) % _num_send_contexts;
				if (_send_contexts[current_send_context]->in_use) {
					break;
				}
			}

			// VLOG("next non-sequential context = %u", current_send_context);
		}
	}

	return ret;
}

//
/// subscribe to a range of stream IDs
//

void CBUSLongMessageEx::subscribe(byte *stream_ids, const byte num_stream_ids, void (*messagehandler)(void *msg, unsigned int msg_len, byte stream_id, byte status)) {

	_stream_ids = stream_ids;
	_num_stream_ids = num_stream_ids;
	_messagehandler = messagehandler;

	// DEBUG_SERIAL << F("> Lex: subscribe: num_stream_ids = ") << num_stream_ids << endl;
	return;
}

//
/// report state of long message sending
//

// return number of streams currently in progress

byte CBUSLongMessageEx::is_sending(void) {

	byte i, num_streams;

	for (i = 0, num_streams = 0; i < _num_send_contexts; i++) {
		if (_send_contexts[i]->in_use) {
			++num_streams;
		}
	}

	return num_streams;
}

// return whether currently sending a message with this stream id

bool CBUSLongMessageEx::is_sending_stream(byte stream_id) {

	for (byte i = 0; i < _num_send_contexts; i++) {
		if (_send_contexts[i]->send_stream_id == stream_id && _send_contexts[i]->in_use) {
			return true;
		}
	}

	return false;
}

//
/// handle an incoming long message CBUS message fragment
//

void CBUSLongMessageEx::processReceivedMessageFragment(const CANFrame *frame) {

	byte i, j, status;
	uint16_t tmpcrc = 0;

	// DEBUG_SERIAL << F("> Lex: handling incoming message fragment") << endl;
	// DEBUG_SERIAL.flush();

	if (frame->data[2] == 0) {																												  // sequence zero = a header fragment with start of new stream
		if (frame->data[7] == 0) {																												// flags = 0, standard message

			// DEBUG_SERIAL << F("> Lex: this is a data message header fragment") << endl;

			for (i = 0; i < _num_stream_ids; i++) {
				if (_stream_ids[i] == frame->data[1]) {																				// are we subscribed to this stream id ?

					// DEBUG_SERIAL << F("> Lex: we are subscribed to this stream ID = ") << frame->data[1] << endl;

					// find a free receive context
					for (i = 0; i < _num_receive_contexts; i++) {
						if (!_receive_contexts[i]->in_use) {
							// DEBUG_SERIAL << F("> Lex: using receive context = ") << i << endl;
							break;
						}
					}

					if (i < _num_receive_contexts) {
						_receive_contexts[i]->in_use = true;
						_receive_contexts[i]->receive_stream_id = frame->data[1];
						_receive_contexts[i]->incoming_message_length = (frame->data[3] << 8) + frame->data[4];
						_receive_contexts[i]->incoming_message_crc = (frame->data[5] << 8) + frame->data[6];
						_receive_contexts[i]->incoming_bytes_received = 0;
						memset(_receive_contexts[i]->buffer, 0, _receive_buffer_len);
						_receive_contexts[i]->receive_buffer_index = 0;
						_receive_contexts[i]->expected_next_receive_sequence_num = 1;
						_receive_contexts[i]->sender_canid = (frame->id & 0x7f);
						_receive_contexts[i]->last_fragment_received = millis();
						// DEBUG_SERIAL << F("> Lex: received header fragment for stream id = ") << _receive_contexts[i]->receive_stream_id << F(", message length = ") << _receive_contexts[i]->incoming_message_length << endl;
					} else {
						// DEBUG_SERIAL << F("> Lex: unable to find free receive context for new message") << endl;
						(void)(*_messagehandler)(nullptr, 0, frame->data[1], CBUS_LONG_MESSAGE_INTERNAL_ERROR);
					}

					break;
				}
			}
		} else {
			// DEBUG_SERIAL << F("> Lex: not handling header fragment with non-zero flags") << endl;
		}
	} else {																																					// continuation fragment

		// DEBUG_SERIAL << F("> Lex: this is a continuation fragment") << endl;

		// find a matching receive context, using the stream ID and sender CANID
		for (i = 0; i < _num_receive_contexts; i++) {
			if (_receive_contexts[i]->in_use && _receive_contexts[i]->receive_stream_id == frame->data[1] && _receive_contexts[i]->sender_canid == (frame->id & 0x7f)) {
				// DEBUG_SERIAL << F("> Lex: found matching receive context = ") << i << endl;
				break;
			}
		}

		// return if not found
		if (i >= _num_receive_contexts) {
			// DEBUG_SERIAL << F("> Lex: did not find matching receive context") << endl;
			return;
		}

		// error if out of sequence
		if (frame->data[2] != _receive_contexts[i]->expected_next_receive_sequence_num) {
			// DEBUG_SERIAL << F("> Lex: ERROR: expected receive sequence num = ") << _receive_contexts[i]->expected_next_receive_sequence_num << F(" but got = ") << frame->data[2] << endl;
			(void)(*_messagehandler)(_receive_contexts[i]->buffer, _receive_contexts[i]->receive_buffer_index, _receive_contexts[i]->receive_stream_id, CBUS_LONG_MESSAGE_SEQUENCE_ERROR);
			_receive_contexts[i]->in_use = false;
			return;
		}

		// consume up to 5 bytes of message data from this fragment
		for (j = 0; j < 5; j++) {
			// DEBUG_SERIAL << F("> Lex: consuming received data byte = ") << (char)frame->data[j + 3] << endl;
			_receive_contexts[i]->buffer[_receive_contexts[i]->receive_buffer_index] = frame->data[j + 3];
			++_receive_contexts[i]->receive_buffer_index;
			++_receive_contexts[i]->incoming_bytes_received;
			_receive_contexts[i]->last_fragment_received = millis();

			// if we have consumed the entire message, surface it to the user's handler
			if (_receive_contexts[i]->incoming_bytes_received >= _receive_contexts[i]->incoming_message_length) {
				// DEBUG_SERIAL << F("> Lex: message data has been fully consumed") << endl;

				if (_use_crc && _receive_contexts[i]->incoming_message_crc != 0) {
					// DEBUG_SERIAL << F("> Lex: calculating CRC16") << endl;
					tmpcrc = crc16((uint8_t *)_receive_contexts[i]->buffer, _receive_contexts[i]->receive_buffer_index);
				}

				if (_receive_contexts[i]->incoming_message_crc != tmpcrc) {
					// DEBUG_SERIAL << F("> Lex: message CRC error, expected = ") << _receive_contexts[i]->incoming_message_crc << F(", calculated = ") << tmpcrc << endl;
					status = CBUS_LONG_MESSAGE_CRC_ERROR;
				} else {
					status = CBUS_LONG_MESSAGE_COMPLETE;
				}

				(void)(*_messagehandler)(_receive_contexts[i]->buffer, _receive_contexts[i]->receive_buffer_index, _receive_contexts[i]->receive_stream_id, status);
				_receive_contexts[i]->in_use = false;
				break;

				// if the buffer is now full, give the user what we have with an error status
			} else if (_receive_contexts[i]->receive_buffer_index >= _receive_buffer_len ) {
				// DEBUG_SERIAL << F("> Lex: buffer is now full, message truncated") << endl;
				(void)(*_messagehandler)(_receive_contexts[i]->buffer, _receive_contexts[i]->receive_buffer_index, _receive_contexts[i]->receive_stream_id, CBUS_LONG_MESSAGE_TRUNCATED);
				_receive_contexts[i]->in_use = false;
				break;
			}
		}

		// increment the expected next sequence number for this stream context
		++_receive_contexts[i]->expected_next_receive_sequence_num;
	}

	return;
}

//
/// set whether to calculate and compare a CRC of the message
//

void CBUSLongMessageEx::use_crc(bool use_crc) {

	_use_crc = use_crc;
	return;
}

void CBUSLongMessageEx::set_sequential(bool state) {

	_is_sequential = state;
	return;
}


///////////////////////////////////////////////////////////////////////////////
//////// CRC implementations

uint32_t crc32(const byte *s, size_t n) {

	uint32_t crc = 0xFFFFFFFF;

	for (size_t i = 0; i < n; i++) {
		uint8_t ch = s[i];
		for (size_t j = 0; j < 8; j++) {
			uint32_t b = (ch ^ crc) & 1;
			crc >>= 1;
			if (b) crc = crc ^ 0xEDB88320;
			ch >>= 1;
		}
	}

	return ~crc;
}

/*
//                                      16   12   5
// this is the CCITT CRC 16 polynomial X  + X  + X  + 1.
// This works out to be 0x1021, but the way the algorithm works
// lets us use 0x8408 (the reverse of the bit pattern).  The high
// bit is always assumed to be set, thus we only use 16 bits to
// represent the 17 bit value.
*/

// http://stjarnhimlen.se/snippets/crc-16.c

#define POLY 0x8408

uint16_t crc16(uint8_t *data_p, uint16_t length) {

	uint8_t i;
	uint16_t data;
	uint16_t crc = 0xffff;

	if (length == 0) {
		return (~crc);
	}

	do {
		for (i = 0, data = (uint16_t)0xff & *data_p++;
		     i < 8;
		     i++, data >>= 1) {
			if ((crc & 0x0001) ^ (data & 0x0001))
				crc = (crc >> 1) ^ POLY;
			else  crc >>= 1;
		}
	} while (--length);

	crc = ~crc;
	data = crc;
	crc = (crc << 8) | (data >> 8 & 0xff);

	return (crc);
}

