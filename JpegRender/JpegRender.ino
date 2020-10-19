/*
 Name:		JpegRender.ino
 Created:	2020-10-18 6:34:57 PM
 Author:	Michael Hotchin
*/

// The MIT License
//
// Copyright 2019-2020 M Hotchin
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.






#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>

//  Replace these with your graphics library
#include <Adafruit_GFX.h>
#include <Waveshare_ILI9486.h>

#include "HtmlColors.h"

#include "tjpgd.h"

struct Point
{
	int16_t x, y;
};


//  Additional graphics methods.  Creating a derived class means I don't have to pass a
//  pointer to the base class to each method.  Replace the base class with the appropriate
//  one from your graphics library.
class ExtendedGFX : public Waveshare_ILI9486
{
public:
	ExtendedGFX();

	//  We render from a Stream because the code was originally written to render from a
	//  URL.  If you *only* want to render from files, the code below would probably be
	//  faster if the input object was a 'File' class instead.
	bool renderJpgFromStream(Stream *pStream, unsigned long size, const Point &ul, bool fCenter = false);
};

ExtendedGFX::ExtendedGFX()
{
};


constexpr unsigned int BUFFER_SIZE = 128;

//  The JPEG decompressor is 'C' based, and uses function pointers to pass in callbacks.
//  In C++, we can use a class object instead, and use class static functions to re-direct
//  to the 'real' class instance functions.
class RenderJpgCallbacks
{
public:
	RenderJpgCallbacks(Stream *, unsigned long size, Adafruit_GFX *);
	~RenderJpgCallbacks();

	static uint16_t jd_input(JDEC *, uint8_t *, uint16_t);
	static uint16_t jd_output(JDEC *, void *, JRECT *);

	void setUpperLeft(const Point &p);
private:
	uint16_t input(uint8_t *, uint16_t);
	uint16_t output(uint16_t *, const JRECT *);

	Stream *_pStream;
	unsigned long _size;
	Point _ul;
	Adafruit_GFX *_pGFX;

	
	uint8_t _buffer[BUFFER_SIZE];
	unsigned int _buffCount = 0;

};


RenderJpgCallbacks::RenderJpgCallbacks(
	Stream *pStream,
	unsigned long size,
	Adafruit_GFX *pGFX)
	: _pStream(pStream), _size(size), _ul({ 0, 0 }), _pGFX(pGFX), _buffCount(0)
{
	
}


RenderJpgCallbacks::~RenderJpgCallbacks()
{
	
}


uint16_t
RenderJpgCallbacks::jd_input(
	JDEC *pjdec,
	uint8_t *pBuffer,
	uint16_t count)
{
	RenderJpgCallbacks *pThis = reinterpret_cast<RenderJpgCallbacks *>(pjdec->device);

	return pThis->input(pBuffer, count);
}


void
RenderJpgCallbacks::setUpperLeft(
	const Point &p)
{
	_ul = p;
}


uint16_t
RenderJpgCallbacks::jd_output(
	JDEC *pjdec,
	void *pBuffer,
	JRECT *jRect)
{
	RenderJpgCallbacks *pThis = reinterpret_cast<RenderJpgCallbacks *>(pjdec->device);

	return pThis->output(reinterpret_cast<uint16_t *>(pBuffer), jRect);
}


//  Streaming from a file like object is easy, the access cost is relatively consistent.
//  For Network objects, managing reads to match available data improves performance
//  considerably.
//
//  When rendering a JPEG, first there are a sequence of small reads, usually less than
//  128 bytes in total.  So, the first read goes into a buffer, and we satisfy from the
//  buffer while we can.  After that, we first copy the remaining buffer into the next
//  read, and fill more directly from the network stream.
//
//  Once the buffer is empty, we satify reads based on how much is available from the
//  stream.  Only if the stream is empty do we issue a 'blocking' read.
uint16_t
RenderJpgCallbacks::input(
	uint8_t *pBuffer,
	uint16_t count)
{
	//Serial.printf("JPG read: %u\n", count);
	if (_size == 0)
	{
		return 0;
	}

	if (count > _size)
		count = _size;

	//  We only satisfy 'small' requests from the buffer.  If there isn't enough in the 
	//  buffer we'll try to fill it up enough to satisfy the request.
	if ((_buffCount < count) && (count < BUFFER_SIZE))
	{
		auto available = _pStream->available();
		if (available > 0)
		{
			auto toRead = min(BUFFER_SIZE - _buffCount, (unsigned int)available);

			//Serial.printf("To Read: %d\n", toRead);
			//delay(10);

			auto read = _pStream->readBytes(_buffer + _buffCount, toRead);

			if (read > 0)
			{
				_buffCount += read;
			}
		}
		//Serial.printf("BuffCount: %u\n", _buffCount);
	}


	//  If pBuffer is null, we are just throwing data away.
	if (pBuffer == nullptr)
	{
		unsigned int i = min(count, _buffCount);

		//  First remove from the buffer.
		_buffCount -= i;

		if (_buffCount != 0)
		{
			// Anything left in the buffer goes to the front.
			memmove(_buffer, _buffer + i, _buffCount);
		}
		else
		{
			auto remaining = count - i;

			if (remaining > 0)
			{
				if (remaining < BUFFER_SIZE)
				{
					auto read = _pStream->readBytes(_buffer, remaining);

					if (read > 0)
					{
						i += read;
					}
				}
				else
				{
					//  Buffer is empty now, so pull it from the network object.  Not worth
					//  optimizing.
					for (; i < count && _pStream->read() > 0; i++)
					{
						//
					}
				}
			}
		}

		_size -= i;
		return i;
	}
	else
	{
		//  First see if there's anything left in the buffer.
		auto amountRead = min(count, _buffCount);

		if (amountRead > 0)
		{
			memcpy(pBuffer, _buffer, amountRead);

			_buffCount -= amountRead;

			if (_buffCount > 0)
			{
				memmove(_buffer, _buffer + amountRead, _buffCount);
			}
		}

		//  Whatever's left to read we pull from the stream.
		auto remaining = count - amountRead;
		if (remaining > 0)
		{
			auto available = _pStream->available();
			if (available > 0)
			{
				auto toRead = min(remaining, (unsigned int)available);

				//  If we have to wait, may as well wait for a reasonable amount
				if (toRead == 0) toRead = remaining;

				//if (toRead != remaining)
				//{
				//	Serial.printf("Wanted %d, trying %d\n", remaining, toRead);
				//}

				auto read = _pStream->readBytes(pBuffer + amountRead, toRead);

				if (read > 0)
				{
					amountRead += read;

					remaining -= read;

					//if (read < toRead)
					//{
					//	Serial.println("Short read!");
					//}

					////  If the read above brought in more than we asked for, see if we can add
					////  more to the buffer.
					//if ((remaining > 0) && (_pStream->available() > 0))
					//{
					//	toRead = min(remaining, _pStream->available());

					//	read = _pStream->readBytes(pBuffer + amountRead, toRead);

					//	if (read > 0)
					//	{
					//		//Serial.printf("Added another %d\n", read);
					//		amountRead += read;
					//	}
					//}
				}
			}
		}

		_size -= amountRead;

		return amountRead;
	}

	// not reached
}


uint16_t
RenderJpgCallbacks::output(
	uint16_t *pBuffer,
	const JRECT *pjrect)
{
	_pGFX->drawRGBBitmap(_ul.x + pjrect->left, _ul.y + pjrect->top,
		pBuffer, pjrect->right - pjrect->left + 1, pjrect->bottom - pjrect->top + 1);

	return 1;
}


bool
ExtendedGFX::renderJpgFromStream(
	Stream *pStream,
	unsigned long size,
	const Point &ref,
	bool fCenter)
{
	//auto tStart = millis();

	RenderJpgCallbacks callbacks(pStream, size, this);
	JDEC jdec;
	constexpr size_t JD_BUFFER_SIZE = 2584 + JD_SZBUF;

#ifdef ARDUINO_ESP32_DEV
	std::unique_ptr<uint8_t[]> allocation{ new uint8_t[JD_BUFFER_SIZE] };
	uint8_t *buffer = allocation.get();
#else
	uint8_t buffer[JD_BUFFER_SIZE];
#endif
	if (jd_prepare(&jdec, &callbacks.jd_input, buffer, JD_BUFFER_SIZE, &callbacks) == JDR_OK)
	{
		if (fCenter)
		{
			callbacks.setUpperLeft({ (int16_t)(ref.x - jdec.width / 2), ref.y });
		}
		else
		{
			callbacks.setUpperLeft(ref);
		}

		auto retval = (jd_decomp(&jdec, &callbacks.jd_output, 0) == JDR_OK);

		//Serial.printf("Render time: %lu\n", millis() - tStart);
		return retval;
	}

	//Serial.println(F("jd_prepare failed!"));

	return false;
}


//  SdFat files (SdFile) are not streams.  This adaptor allows a file to be used as a
//  stream.
class SdFileStream : public Stream
{
public:
	SdFileStream(SdFile &file);
	virtual int available();
	virtual int read();
	
	virtual size_t write(uint8_t);

	//  We don't need 'peek()' for our purposes here.
	virtual int peek() {
		return -1;
	};
private:
	SdFile &_file;
};


SdFileStream::SdFileStream(
	SdFile &file)
	: _file(file)
{}


int 
SdFileStream::available()
{
	return _file.fileSize()- _file.curPosition();
}

int
SdFileStream::read()
{
	return _file.read();
}


size_t
SdFileStream::write(uint8_t b)
{
	return _file.write(b);
}


ExtendedGFX tft;
SdFat SD;

// the setup function runs once when you press reset or power the board
void setup() 
{
	Serial.begin(115200);

	while (!Serial);

	//  You should set the chip select for all SPI devices HIGH before intializing things.
	//  In our case, the Wavehshare library does so.

	SPI.begin();

	tft.begin();

	tft.fillScreen(RGB565::Black);
	tft.setRotation(1);
	tft.setTextSize(2);

	tft.println(F("JPEG Rendering"));

	tft.setTextSize(1);

	//  This may take some timre for large cards.
	if (!SD.begin(tft.GetSdCardCS()))
	{
		tft.println(F("Unable to init SD card!"));
	}

}

// the loop function runs over and over again until power down or reset
void loop() 
{
	SdFile file;

	if (!file.open("TEST.JPG"))
	{
		tft.println(F("Unable to open 'TEST.JPG'."));
	}
	else
	{
		SdFileStream fs(file);

		tft.renderJpgFromStream(&fs, file.fileSize(), {tft.width()/2, 20}, true);
	}

	delay(3000);

}
