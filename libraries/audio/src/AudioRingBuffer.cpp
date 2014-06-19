//
//  AudioRingBuffer.cpp
//  libraries/audio/src
//
//  Created by Stephen Birarda on 2/1/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <cstring>
#include <functional>
#include <math.h>
#include "SharedUtil.h"

#include <QtCore/QDebug>

#include "PacketHeaders.h"

#include "AudioRingBuffer.h"

InterframeTimeGapHistory::InterframeTimeGapHistory()
    : _lastFrameReceivedTime(0),
    _numSamplesInCurrentInterval(0),
    _currentIntervalMaxGap(0),
    _newestIntervalMaxGapAt(0),
    _windowMaxGap(0),
    _newWindowMaxGapAvailable(false)
{
    memset(_intervalMaxGaps, 0, TIME_GAP_NUM_INTERVALS_IN_WINDOW*sizeof(quint64));
}

void InterframeTimeGapHistory::frameReceived() {
    quint64 now = usecTimestampNow();

    // make sure this isn't the first time frameReceived() is called, meaning there's actually a gap to calculate.
    if (_lastFrameReceivedTime != 0) {
        quint64 gap = now - _lastFrameReceivedTime;

printf("new gap: %llu\n", gap);

        // update the current interval max
        if (gap > _currentIntervalMaxGap) {
            _currentIntervalMaxGap = gap;
        }
        _numSamplesInCurrentInterval++;

        // if the current interval of samples is now full, record it in our interval maxes
        if (_numSamplesInCurrentInterval == TIME_GAP_NUM_SAMPLES_IN_INTERVAL) {

printf("\t interval full: max interval gap: %llu\n", _currentIntervalMaxGap);

            // find location to insert this interval's max (increment index cyclically)
            _newestIntervalMaxGapAt = _newestIntervalMaxGapAt == TIME_GAP_NUM_INTERVALS_IN_WINDOW - 1 ? 0 : _newestIntervalMaxGapAt + 1;

            // record the current interval's max gap as the newest
            _intervalMaxGaps[_newestIntervalMaxGapAt] = _currentIntervalMaxGap;

            // update the window max gap, which is the max out of all the past intervals' max gaps
            _windowMaxGap = 0;
            for (int i = 0; i < TIME_GAP_NUM_INTERVALS_IN_WINDOW; i++) {
                if (_intervalMaxGaps[i] > _windowMaxGap) {
                    _windowMaxGap = _intervalMaxGaps[i];
                }
            }
            _newWindowMaxGapAvailable = true;

printf("\t\t new window max gap: %llu\n", _windowMaxGap);

            // reset the current interval
            _numSamplesInCurrentInterval = 0;
            _currentIntervalMaxGap = 0;
        }
    }
    _lastFrameReceivedTime = now;
}

quint64 InterframeTimeGapHistory::getPastWindowMaxGap() {
    _newWindowMaxGapAvailable = false;
    return _windowMaxGap;
}


AudioRingBuffer::AudioRingBuffer(int numFrameSamples, bool randomAccessMode) :
    NodeData(),
    _sampleCapacity(numFrameSamples * RING_BUFFER_LENGTH_FRAMES),
    _numFrameSamples(numFrameSamples),
    _isStarved(true),
    _hasStarted(false),
    _randomAccessMode(randomAccessMode)
{
    if (numFrameSamples) {
        _buffer = new int16_t[_sampleCapacity];
        if (_randomAccessMode) {
            memset(_buffer, 0, _sampleCapacity * sizeof(int16_t));
        }
        _nextOutput = _buffer;
        _endOfLastWrite = _buffer;
    } else {
        _buffer = NULL;
        _nextOutput = NULL;
        _endOfLastWrite = NULL;
    }
};

AudioRingBuffer::~AudioRingBuffer() {
    delete[] _buffer;
}

void AudioRingBuffer::reset() {
    _endOfLastWrite = _buffer;
    _nextOutput = _buffer;
    _isStarved = true;
}

void AudioRingBuffer::resizeForFrameSize(qint64 numFrameSamples) {
    delete[] _buffer;
    _sampleCapacity = numFrameSamples * RING_BUFFER_LENGTH_FRAMES;
    _buffer = new int16_t[_sampleCapacity];
    if (_randomAccessMode) {
        memset(_buffer, 0, _sampleCapacity * sizeof(int16_t));
    }
    _nextOutput = _buffer;
    _endOfLastWrite = _buffer;
}

int AudioRingBuffer::parseData(const QByteArray& packet) {
    int numBytesPacketHeader = numBytesForPacketHeader(packet);
    return writeData(packet.data() + numBytesPacketHeader, packet.size() - numBytesPacketHeader);
}

qint64 AudioRingBuffer::readSamples(int16_t* destination, qint64 maxSamples) {
    return readData((char*) destination, maxSamples * sizeof(int16_t));
}

qint64 AudioRingBuffer::readData(char *data, qint64 maxSize) {

    // only copy up to the number of samples we have available
    int numReadSamples = std::min((unsigned) (maxSize / sizeof(int16_t)), samplesAvailable());

    // If we're in random access mode, then we consider our number of available read samples slightly
    // differently. Namely, if anything has been written, we say we have as many samples as they ask for
    // otherwise we say we have nothing available
    if (_randomAccessMode) {
        numReadSamples = _endOfLastWrite ? (maxSize / sizeof(int16_t)) : 0;
    }

    if (_nextOutput + numReadSamples > _buffer + _sampleCapacity) {
        // we're going to need to do two reads to get this data, it wraps around the edge

        // read to the end of the buffer
        int numSamplesToEnd = (_buffer + _sampleCapacity) - _nextOutput;
        memcpy(data, _nextOutput, numSamplesToEnd * sizeof(int16_t));
        if (_randomAccessMode) {
            memset(_nextOutput, 0, numSamplesToEnd * sizeof(int16_t)); // clear it
        }
        
        // read the rest from the beginning of the buffer
        memcpy(data + (numSamplesToEnd * sizeof(int16_t)), _buffer, (numReadSamples - numSamplesToEnd) * sizeof(int16_t));
        if (_randomAccessMode) {
            memset(_buffer, 0, (numReadSamples - numSamplesToEnd) * sizeof(int16_t)); // clear it
        }
    } else {
        // read the data
        memcpy(data, _nextOutput, numReadSamples * sizeof(int16_t));
        if (_randomAccessMode) {
            memset(_nextOutput, 0, numReadSamples * sizeof(int16_t)); // clear it
        }
    }

    // push the position of _nextOutput by the number of samples read
    _nextOutput = shiftedPositionAccomodatingWrap(_nextOutput, numReadSamples);

    return numReadSamples * sizeof(int16_t);
}

qint64 AudioRingBuffer::writeSamples(const int16_t* source, qint64 maxSamples) {    
    return writeData((const char*) source, maxSamples * sizeof(int16_t));
}

qint64 AudioRingBuffer::writeData(const char* data, qint64 maxSize) {
    // make sure we have enough bytes left for this to be the right amount of audio
    // otherwise we should not copy that data, and leave the buffer pointers where they are

    int samplesToCopy = std::min((quint64)(maxSize / sizeof(int16_t)), (quint64)_sampleCapacity);

    std::less<int16_t*> less;
    std::less_equal<int16_t*> lessEqual;

    if (_hasStarted
        && (less(_endOfLastWrite, _nextOutput)
            && lessEqual(_nextOutput, shiftedPositionAccomodatingWrap(_endOfLastWrite, samplesToCopy)))) {
        // this read will cross the next output, so call us starved and reset the buffer
        qDebug() << "Filled the ring buffer. Resetting.";
        _endOfLastWrite = _buffer;
        _nextOutput = _buffer;
        _isStarved = true;
    }

    if (_endOfLastWrite + samplesToCopy <= _buffer + _sampleCapacity) {
        memcpy(_endOfLastWrite, data, samplesToCopy * sizeof(int16_t));
    } else {
        int numSamplesToEnd = (_buffer + _sampleCapacity) - _endOfLastWrite;
        memcpy(_endOfLastWrite, data, numSamplesToEnd * sizeof(int16_t));
        memcpy(_buffer, data + (numSamplesToEnd * sizeof(int16_t)), (samplesToCopy - numSamplesToEnd) * sizeof(int16_t));
    }

    _endOfLastWrite = shiftedPositionAccomodatingWrap(_endOfLastWrite, samplesToCopy);

    return samplesToCopy * sizeof(int16_t);
}

int16_t& AudioRingBuffer::operator[](const int index) {
    return *shiftedPositionAccomodatingWrap(_nextOutput, index);
}

const int16_t& AudioRingBuffer::operator[] (const int index) const {
    return *shiftedPositionAccomodatingWrap(_nextOutput, index);
}

void AudioRingBuffer::shiftReadPosition(unsigned int numSamples) {
    _nextOutput = shiftedPositionAccomodatingWrap(_nextOutput, numSamples);
}

unsigned int AudioRingBuffer::samplesAvailable() const {
    if (!_endOfLastWrite) {
        return 0;
    } else {
        int sampleDifference = _endOfLastWrite - _nextOutput;

        if (sampleDifference < 0) {
            sampleDifference += _sampleCapacity;
        }

        return sampleDifference;
    }
}

void AudioRingBuffer::addSilentFrame(int numSilentSamples) {
    // memset zeroes into the buffer, accomodate a wrap around the end
    // push the _endOfLastWrite to the correct spot
    if (_endOfLastWrite + numSilentSamples <= _buffer + _sampleCapacity) {
        memset(_endOfLastWrite, 0, numSilentSamples * sizeof(int16_t));
        _endOfLastWrite += numSilentSamples;
    } else {
        int numSamplesToEnd = (_buffer + _sampleCapacity) - _endOfLastWrite;
        memset(_endOfLastWrite, 0, numSamplesToEnd * sizeof(int16_t));
        memset(_buffer, 0, (numSilentSamples - numSamplesToEnd) * sizeof(int16_t));
        
        _endOfLastWrite = _buffer + (numSilentSamples - numSamplesToEnd);
    }
}

bool AudioRingBuffer::isNotStarvedOrHasMinimumSamples(unsigned int numRequiredSamples) const {
    if (!_isStarved) {
        return true;
    } else {
        return samplesAvailable() >= numRequiredSamples;
    }
}

int16_t* AudioRingBuffer::shiftedPositionAccomodatingWrap(int16_t* position, int numSamplesShift) const {

    if (numSamplesShift > 0 && position + numSamplesShift >= _buffer + _sampleCapacity) {
        // this shift will wrap the position around to the beginning of the ring
        return position + numSamplesShift - _sampleCapacity;
    } else if (numSamplesShift < 0 && position + numSamplesShift < _buffer) {
        // this shift will go around to the end of the ring
        return position + numSamplesShift + _sampleCapacity;
    } else {
        return position + numSamplesShift;
    }
}
