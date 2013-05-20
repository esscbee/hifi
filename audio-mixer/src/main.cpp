//
//  main.cpp
//  mixer
//
//  Created by Stephen Birarda on 2/1/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#include <iostream>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <fstream>
#include <limits>
#include <signal.h>

#include <AgentList.h>
#include <Agent.h>
#include <AgentTypes.h>
#include <SharedUtil.h>
#include <StdDev.h>

#include "AudioRingBuffer.h"
#include "PacketHeaders.h"

#ifdef _WIN32
#include "Syssocket.h"
#include "Systime.h"
#include <math.h>
#else
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif //_WIN32

const unsigned short MIXER_LISTEN_PORT = 55443;

const float SAMPLE_RATE = 22050.0;

const short JITTER_BUFFER_MSECS = 12;
const short JITTER_BUFFER_SAMPLES = JITTER_BUFFER_MSECS * (SAMPLE_RATE / 1000.0);

const int BUFFER_LENGTH_BYTES = 1024;
const int BUFFER_LENGTH_SAMPLES_PER_CHANNEL = (BUFFER_LENGTH_BYTES / 2) / sizeof(int16_t);

const short RING_BUFFER_FRAMES = 10;
const short RING_BUFFER_SAMPLES = RING_BUFFER_FRAMES * BUFFER_LENGTH_SAMPLES_PER_CHANNEL;

const float BUFFER_SEND_INTERVAL_USECS = (BUFFER_LENGTH_SAMPLES_PER_CHANNEL / SAMPLE_RATE) * 1000000;

const long MAX_SAMPLE_VALUE = std::numeric_limits<int16_t>::max();
const long MIN_SAMPLE_VALUE = std::numeric_limits<int16_t>::min();

const float DISTANCE_RATIO = 3.0f / 0.3f;
const float PHASE_AMPLITUDE_RATIO_AT_90 = 0.5;
const int PHASE_DELAY_AT_90 = 20;

const float MAX_OFF_AXIS_ATTENUATION = 0.2f;
const float OFF_AXIS_ATTENUATION_FORMULA_STEP = (1 - MAX_OFF_AXIS_ATTENUATION) / 2.0f;

void plateauAdditionOfSamples(int16_t &mixSample, int16_t sampleToAdd) {
    long sumSample = sampleToAdd + mixSample;
    
    long normalizedSample = std::min(MAX_SAMPLE_VALUE, sumSample);
    normalizedSample = std::max(MIN_SAMPLE_VALUE, sumSample);
    
    mixSample = normalizedSample;    
}

void attachNewBufferToAgent(Agent *newAgent) {
    if (!newAgent->getLinkedData()) {
        newAgent->setLinkedData(new AudioRingBuffer(RING_BUFFER_SAMPLES, BUFFER_LENGTH_SAMPLES_PER_CHANNEL));
    }
}

int main(int argc, const char* argv[]) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    
    AgentList* agentList = AgentList::createInstance(AGENT_TYPE_AUDIO_MIXER, MIXER_LISTEN_PORT);
    
    ssize_t receivedBytes = 0;
    
    agentList->linkedDataCreateCallback = attachNewBufferToAgent;
    
    agentList->startSilentAgentRemovalThread();
    agentList->startDomainServerCheckInThread();

    unsigned char* packetData = new unsigned char[MAX_PACKET_SIZE];

    sockaddr* agentAddress = new sockaddr;

    // make sure our agent socket is non-blocking
    agentList->getAgentSocket()->setBlocking(false);
    
    int nextFrame = 0;
    timeval startTime;
    
    unsigned char clientPacket[BUFFER_LENGTH_BYTES + 1];
    clientPacket[0] = PACKET_HEADER_MIXED_AUDIO;
    
    int16_t clientSamples[BUFFER_LENGTH_SAMPLES_PER_CHANNEL * 2] = {};
    
    gettimeofday(&startTime, NULL);
    
    while (true) {
        // enumerate the agents, check if we can add audio from the agent to current mix
        for (AgentList::iterator agent = agentList->begin(); agent != agentList->end(); agent++) {
            AudioRingBuffer* agentBuffer = (AudioRingBuffer*) agent->getLinkedData();
            
            if (agentBuffer->getEndOfLastWrite()) {
                if (!agentBuffer->isStarted()
                    && agentBuffer->diffLastWriteNextOutput() <= BUFFER_LENGTH_SAMPLES_PER_CHANNEL + JITTER_BUFFER_SAMPLES) {
                    printf("Held back buffer for agent with ID %d.\n", agent->getAgentID());
                    agentBuffer->setShouldBeAddedToMix(false);
                } else if (agentBuffer->diffLastWriteNextOutput() < BUFFER_LENGTH_SAMPLES_PER_CHANNEL) {
                    printf("Buffer from agent with ID %d starved.\n", agent->getAgentID());
                    agentBuffer->setStarted(false);
                    agentBuffer->setShouldBeAddedToMix(false);
                } else {
                    // good buffer, add this to the mix
                    agentBuffer->setStarted(true);
                    agentBuffer->setShouldBeAddedToMix(true);
                }
            }
        }
        
        int numAgents = agentList->size();
        float distanceCoefficients[numAgents][numAgents];
        memset(distanceCoefficients, 0, sizeof(distanceCoefficients));
        
        for (AgentList::iterator agent = agentList->begin(); agent != agentList->end(); agent++) {
            if (agent->getType() == AGENT_TYPE_AVATAR) {
                AudioRingBuffer* agentRingBuffer = (AudioRingBuffer*) agent->getLinkedData();
                
                // zero out the client mix for this agent
                memset(clientSamples, 0, sizeof(clientSamples));
                
                for (AgentList::iterator otherAgent = agentList->begin(); otherAgent != agentList->end(); otherAgent++) {
                    if (otherAgent != agent || (otherAgent == agent && agentRingBuffer->shouldLoopbackForAgent())) {
                        AudioRingBuffer* otherAgentBuffer = (AudioRingBuffer*) otherAgent->getLinkedData();
                        
                        if (otherAgentBuffer->shouldBeAddedToMix()) {
                            
                            float bearingRelativeAngleToSource = 0.f;
                            float attenuationCoefficient = 1.f;
                            int numSamplesDelay = 0;
                            float weakChannelAmplitudeRatio = 1.f;
                            
                            if (otherAgent != agent) {
                                glm::vec3 agentPosition = agentRingBuffer->getPosition();
                                glm::vec3 otherAgentPosition = otherAgentBuffer->getPosition();
                                
                                // calculate the distance to the other agent
                                
                                // use the distance to the other agent to calculate the change in volume for this frame
                                int lowAgentIndex = std::min(agent.getAgentIndex(), otherAgent.getAgentIndex());
                                int highAgentIndex = std::max(agent.getAgentIndex(), otherAgent.getAgentIndex());
                                
                                if (distanceCoefficients[lowAgentIndex][highAgentIndex] == 0) {
                                    float distanceToAgent = sqrtf(powf(agentPosition.x - otherAgentPosition.x, 2) +
                                                                  powf(agentPosition.y - otherAgentPosition.y, 2) +
                                                                  powf(agentPosition.z - otherAgentPosition.z, 2));
                                    
                                    float minCoefficient = std::min(1.0f,
                                                                    powf(0.5,
                                                                         (logf(DISTANCE_RATIO * distanceToAgent) / logf(3)) - 1));
                                    distanceCoefficients[lowAgentIndex][highAgentIndex] = minCoefficient;
                                }
                                
                                
                                // get the angle from the right-angle triangle
                                float triangleAngle = atan2f(fabsf(agentPosition.z - otherAgentPosition.z),
                                                             fabsf(agentPosition.x - otherAgentPosition.x)) * (180 / M_PI);
                                float absoluteAngleToSource = 0;
                                bearingRelativeAngleToSource = 0;
                                
                                // find the angle we need for calculation based on the orientation of the triangle
                                if (otherAgentPosition.x > agentPosition.x) {
                                    if (otherAgentPosition.z > agentPosition.z) {
                                        absoluteAngleToSource = -90 + triangleAngle;
                                    } else {
                                        absoluteAngleToSource = -90 - triangleAngle;
                                    }
                                } else {
                                    if (otherAgentPosition.z > agentPosition.z) {
                                        absoluteAngleToSource = 90 - triangleAngle;
                                    } else {
                                        absoluteAngleToSource = 90 + triangleAngle;
                                    }
                                }
                                
                                bearingRelativeAngleToSource = absoluteAngleToSource - agentRingBuffer->getBearing();
                                
                                if (bearingRelativeAngleToSource > 180) {
                                    bearingRelativeAngleToSource -= 360;
                                } else if (bearingRelativeAngleToSource < -180) {
                                    bearingRelativeAngleToSource += 360;
                                }
                                
                                float angleOfDelivery = absoluteAngleToSource - otherAgentBuffer->getBearing();
                                
                                if (angleOfDelivery > 180) {
                                    angleOfDelivery -= 360;
                                } else if (angleOfDelivery < -180) {
                                    angleOfDelivery += 360;
                                }
                                
                                float offAxisCoefficient = MAX_OFF_AXIS_ATTENUATION +
                                (OFF_AXIS_ATTENUATION_FORMULA_STEP * (fabsf(angleOfDelivery) / 90.0f));
                                
                                attenuationCoefficient = distanceCoefficients[lowAgentIndex][highAgentIndex]
                                * otherAgentBuffer->getAttenuationRatio()
                                * offAxisCoefficient;
                                
                                bearingRelativeAngleToSource *= (M_PI / 180);
                                
                                float sinRatio = fabsf(sinf(bearingRelativeAngleToSource));
                                numSamplesDelay = PHASE_DELAY_AT_90 * sinRatio;
                                weakChannelAmplitudeRatio = 1 - (PHASE_AMPLITUDE_RATIO_AT_90 * sinRatio);
                            }
                            
                            int16_t* goodChannel = bearingRelativeAngleToSource > 0.0f
                            ? clientSamples + BUFFER_LENGTH_SAMPLES_PER_CHANNEL
                            : clientSamples;
                            int16_t* delayedChannel = bearingRelativeAngleToSource > 0.0f
                            ? clientSamples
                            : clientSamples + BUFFER_LENGTH_SAMPLES_PER_CHANNEL;
                            
                            int16_t* delaySamplePointer = otherAgentBuffer->getNextOutput() == otherAgentBuffer->getBuffer()
                            ? otherAgentBuffer->getBuffer() + RING_BUFFER_SAMPLES - numSamplesDelay
                            : otherAgentBuffer->getNextOutput() - numSamplesDelay;
                            
                            for (int s = 0; s < BUFFER_LENGTH_SAMPLES_PER_CHANNEL; s++) {
                                
                                if (s < numSamplesDelay) {
                                    // pull the earlier sample for the delayed channel
                                    int earlierSample = delaySamplePointer[s] * attenuationCoefficient;
                                    plateauAdditionOfSamples(delayedChannel[s], earlierSample * weakChannelAmplitudeRatio);
                                }
                                
                                int16_t currentSample = (otherAgentBuffer->getNextOutput()[s] * attenuationCoefficient);
                                plateauAdditionOfSamples(goodChannel[s], currentSample);
                                
                                if (s + numSamplesDelay < BUFFER_LENGTH_SAMPLES_PER_CHANNEL) {
                                    plateauAdditionOfSamples(delayedChannel[s + numSamplesDelay],
                                                             currentSample * weakChannelAmplitudeRatio);
                                }
                            }
                        }
                    }
                }
                
                memcpy(clientPacket + 1, clientSamples, sizeof(clientSamples));
                agentList->getAgentSocket()->send(agent->getPublicSocket(), clientPacket, BUFFER_LENGTH_BYTES + 1);
            }
        }
        
        // push forward the next output pointers for any audio buffers we used
        for (AgentList::iterator agent = agentList->begin(); agent != agentList->end(); agent++) {
            AudioRingBuffer* agentBuffer = (AudioRingBuffer*) agent->getLinkedData();
            if (agentBuffer && agentBuffer->shouldBeAddedToMix()) {
                agentBuffer->setNextOutput(agentBuffer->getNextOutput() + BUFFER_LENGTH_SAMPLES_PER_CHANNEL);
                
                if (agentBuffer->getNextOutput() >= agentBuffer->getBuffer() + RING_BUFFER_SAMPLES) {
                    agentBuffer->setNextOutput(agentBuffer->getBuffer());
                }
                
                agentBuffer->setShouldBeAddedToMix(false);
            }
        }
        
        // pull any new audio data from agents off of the network stack
        while (agentList->getAgentSocket()->receive(agentAddress, packetData, &receivedBytes)) {
            if (packetData[0] == PACKET_HEADER_MICROPHONE_AUDIO) {
                Agent* avatarAgent = agentList->addOrUpdateAgent(agentAddress,
                                                                 agentAddress,
                                                                 AGENT_TYPE_AVATAR,
                                                                 agentList->getLastAgentID());
                
                if (avatarAgent->getAgentID() == agentList->getLastAgentID()) {
                    agentList->increaseAgentID();
                }
                
                agentList->updateAgentWithData(agentAddress, packetData, receivedBytes);
            } else if (packetData[0] == PACKET_HEADER_INJECT_AUDIO) {
                Agent* matchingInjector = NULL;
                
                for (AgentList::iterator agent = agentList->begin(); agent != agentList->end(); agent++) {
                    if (agent->getLinkedData()) {
                       
                        AudioRingBuffer* ringBuffer = (AudioRingBuffer*) agent->getLinkedData();
                        if (memcmp(ringBuffer->getStreamIdentifier(),
                                   packetData + 1,
                                   STREAM_IDENTIFIER_NUM_BYTES) == 0) {
                            // this is the matching stream, assign to matchingInjector and stop looking
                            matchingInjector = &*agent;
                            break;
                        }
                    }
                }
                
                if (!matchingInjector) {
                    matchingInjector = agentList->addOrUpdateAgent(NULL,
                                                                   NULL,
                                                                   AGENT_TYPE_AUDIO_INJECTOR,
                                                                   agentList->getLastAgentID());
                    agentList->increaseAgentID();
                    
                }
                
                // give the new audio data to the matching injector agent
                agentList->updateAgentWithData(matchingInjector, packetData, receivedBytes);
            }
        }
        
        double usecToSleep = usecTimestamp(&startTime) + (++nextFrame * BUFFER_SEND_INTERVAL_USECS) - usecTimestampNow();
        
        if (usecToSleep > 0) {
            usleep(usecToSleep);
        } else {
            std::cout << "Took too much time, not sleeping!\n";
        }
    }
    
    return 0;
}
