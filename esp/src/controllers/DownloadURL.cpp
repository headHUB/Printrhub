/*
 * This is the base class for all URL downloading classes. It handles all the
 * request and response details and calls a few functions that must be
 * overridden by the subclass to handle incoming data and errors.
 *
 * More Info and documentation:
 * http://www.appfruits.com/2016/11/behind-the-scenes-printrbot-simple-2016/
 *
 * Copyright (c) 2016 Printrbot Inc.
 * Author: Phillip Schuster
 * https://github.com/Printrbot/Printrhub
 *
 * Developed in cooperation by Phillip Schuster (@appfruits) from appfruits.com
 * http://www.appfruits.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


#include "DownloadURL.h"
#include "Application.h"
#include "Idle.h"
#include "../core/CommStack.h"

// Number of milliseconds to wait without receiving any data before we give up
const int kNetworkTimeout = 10 * 1000;
// Number of milliseconds to wait if no data is available before trying again
const int kNetworkDelay = 100;

DownloadURL::DownloadURL(String url) :
	Mode(),
	mode(StateRequest),
	_url(url),
	_stream(NULL) {
  memset(_buffer, 0, _bufferSize);
  _numChunks = 0;
}

DownloadURL::~DownloadURL() {
}

bool DownloadURL::parseUrl() {
  // parse protocol
  int i = _url.indexOf(":");
  if (i < 0) return false;

  _protocol = _url.substring(0, i);
  _url.remove(0, (i + 3));

  // parse host
  i = _url.indexOf('/');
  _host = _url.substring(0, i);
  _url.remove(0, i);

  // parse port
  i = _host.indexOf(':');
  if (i > 0) {
	EventLogger::log("EXTRACTING PORT");
	EventLogger::log(_host.substring(i + 1).c_str());
	_port = _host.substring(i + 1).toInt();
	_host.remove(i);
  } else {
	_port = 80;
  }
  // parse path
  _path = _url;

  EventLogger::log(_protocol.c_str());
  EventLogger::log(_host.c_str());
  EventLogger::log(_url.c_str());

  char _p[6];
  sprintf(_p, "%05i", _port);
  EventLogger::log(_p);

  return true;
}

bool DownloadURL::readNextData() {
  return true;
}

void DownloadURL::loop() {
  int err = 0;

  //In this mode ESP will try to initiate the download of the requested file and will send the response with number of bytes to download
  if (mode == StateRequest) {
	if (!_httpClient.begin(_url)) {
	  EventLogger::log("Download failed, could not parse URL: %s", _url.c_str());
	  mode = StateError;
	  _error = DownloadError::ConnectionFailed;
	} else {
	  err = _httpClient.GET();
	  if (err >= 200 && err <= 299) {
		_stream = _httpClient.getStreamPtr();
		_bytesToDownload = _httpClient.getSize();

		EventLogger::log("Begin Download from %s, size: %d", _url.c_str(), _bytesToDownload);

		if (_stream == NULL || _bytesToDownload == 0) {
		  mode = StateError;
		  _error = DownloadError::UnknownError;
		} else {
		  onBeginDownload(_bytesToDownload);

		  mode = StateDownload;
		  _lastBytesReadTimeStamp = millis();
		}
	  } else if (err >= 300 && err <= 399) {
		mode = StateError;
		_error = DownloadError::Forbidden;
	  } else if (err >= 400 && err <= 499) {
		mode = StateError;
		_error = DownloadError::FileNotFound;
	  } else if (err >= 500 && err <= 599) {
		mode = StateError;
		_error = DownloadError::InternalServerError;
	  } else {
		mode = StateError;
		_error = DownloadError::UnknownError;
	  }
	}
  }

  //In this mode ESP will download the file by chunks of _bufferSize (32 bytes) and will then leave the loop to allow for responses
  if (mode == StateDownload) {
	//Check if we have to wait until the last data have been processed
	if (!readNextData()) {
	  return;
	}

	while (_httpClient.connected() && (_bytesToDownload > 0 || _bytesToDownload == -1)) {
	  // get available data size
	  size_t size = _stream->available();
	  if (size) {
		// read up to 128 byte
		int c = _stream->readBytes(_buffer, ((size > _bufferSize) ? _bufferSize : size));

		//EventLogger::log("Data received, size: %d, bytes left: %d",c,_bytesToDownload);
		if (c > 0) {
		  onDataReceived(_buffer, c);
		  _bytesToDownload -= c;
		}

		_lastBytesReadTimeStamp = millis();

		//Close this run loop now to keep up the rest of the application loop and to wait for the response
		return;
	  } else {
		//Check for timeout
		if ((millis() - _lastBytesReadTimeStamp) > kNetworkTimeout) {
		  EventLogger::log("Download failed, timeout");

		  LOG("Connection timeout");
		  mode = StateError;
		  _error = DownloadError::Timeout;
		  return;
		}

		delay(1);
	  }
	}

	if (_bytesToDownload == 0) {
	  mode = StateSuccess;
	} else {
	  mode = StateError;
	  _error = DownloadError::Timeout;
	}
  }

  if (mode == StateError) {
	//Send the error to MK20
	EventLogger::log("Download failed, Error-Code: %d", _error);
	onError(_error);
	return;
  }

  if (mode == StateSuccess) {
	EventLogger::log("Download successful");
	onFinished();
	return;
  }

  if (mode == StateCancelled) {
	EventLogger::log("Download cancelled");
	onCancelled();
	return;
  }
}

void DownloadURL::cancelDownload() {
  mode = StateCancelled;
}

bool DownloadURL::handlesTask(TaskID taskID) {
  return Mode::handlesTask(taskID);
}

bool DownloadURL::runTask(CommHeader &header, const uint8_t *data, size_t dataSize, uint8_t *responseData, uint16_t *responseDataSize, bool *sendResponse, bool *success) {
  return Mode::runTask(header, data, dataSize, responseData, responseDataSize, sendResponse, success);
}