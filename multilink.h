/*
 * Copyright 2020 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Written by: Andrey Belogolovy
 *             e-mail: andrey.belogolovy@intel.com
 */

#ifndef MULTILINK_H
#define MULTILINK_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>

class MultiLink {

public:
  MultiLink();
  ~MultiLink();

  /**
   * This function needs to be called first.
   * It sets up parameters, allocates memory and starts threads.
   * @param n_local is the number of local communication devices, i.e. pairs of
   * (interface:port)
   * @param n_remote is the number of remote communication devices, i.e. pairs
   * of (ip:port)
   */
  void setCommDeviceNumber(uint16_t NumLocal, uint16_t NumRemote);

  /**
   * A wrapper on setLocalIfaceAndPort automatically incrementing index
   * and checking to not exceed maximum allowed number of links.
   * The function assumes that memory is allocated for Name and it interrupts
   * with '\0'.
   */
  void addLocalIfaceAndPort(char *Name, uint16_t Port);

  /**
   * A wrapper on setRemoteAddrAndPort automatically incrementing num
   * and checking to not exceed maximum allowed number of links.
   * The function assumes that memory is allocated for Name and it interrupts
   * with '\0'.
   */
  void addRemoteAddrAndPort(char *Name, uint16_t Port);

  /**
   * Itiniates connections by starting handshake protocol. Returns immediately.
   * Real connection establishing may take some time.
   * Number of links that have successfully established connections can be known
   * by calling getActiveLinksNumber()
   */
  void initiateLinks();

  /**
   * Returns the number of links that have successfully passed the handshake
   * protocol and ready to send/receive data
   * If return number ==0, there will be no actual data transmission
   */
  uint16_t getActiveLinksNumber();

  /**
   * Sends a data block (also referred to as a sequence) to the remote side.
   * Does not block. If there is no links with established connections, does not
   * actually transmit data.
   * @param Size is the length of the sequence. Maximum size is limited by
   * (65535-6) as 6 bytes are needed for an extra header
   * @param *DataBuf is the pointer to the actual data
   */
  void send(uint16_t Size, uint8_t *DataBuf);

  /**
   * Receives a data block (also referred to as a sequence) from the remote
   * side.
   * Blocks until a sequence to receive becomes available.
   * @param Size is a pointer to a size of the received seqience. Maximum size
   * is limited by (65535-6)
   * @param DataBuf is a pointer to the actual data. Needs to be allocated by a
   * caller.
   */
  void receive(uint16_t *Size, uint8_t *DataBuf);

  /**
   * Sets FEC coding parameters, i.e. the number of redundant packets to be
   * generated.
   * For each K input packets, (N - K) redundant packets will be generated.
   * @param K is the message length of the code
   * @param N is the codeword length of the code
   */
  void setRedundancy(uint8_t K, uint8_t N);

  /**
   * Returns the desired transmission rate.
   */
  uint64_t getRate();

  /**
   * Returns current time.
   */
  uint64_t getLocalTime();

  /**
   * Prints link information.
   */
  void printLinksInfo();

private:
  uint16_t TopLocalLink; // Number of the latest link set up
  uint16_t TopRemoteLink; // Number of the latest link set up

  // reference only. Never used
  struct SequenceT {
    uint16_t PayloadSize;
    uint32_t Crc;
    uint8_t *Data;
  } * Sequence;

  // reference only. Never used
  struct PaketT {
    uint16_t SequenceId;
    uint8_t PacketIdx; // current packet index, max = 255 here, but CRS lets to
                        // have 256
    uint8_t OriginalPacketNumber;
    uint8_t RecoveryPacketNumber;
    uint8_t *Data;
  } * Packet;

  // link is a structure to store all data about one connection
  struct LinkT {
    uint16_t Status; // 0 = link down, 1 = handshake initiated, 2 = handshare in
                     // progress, 3 = link is up
    double RxMeasuredRate, TxMeasuredRate;
    uint16_t
        RxReportedRate; // rate as reported by the receiver. Rate is in KBps
    float RxRateStatistics[10],
        TxRateStatistics[10]; // [0] = newest, [1] = previous, [9] = oldest.
                                // Rates are in Kbps
    uint64_t TxDesiredRate;   // target rate
    uint64_t TxRateBeforeProbation;
    uint64_t LastRateControlActionTime;    // last time rate change requested
    uint64_t LastMeasurementsReceivedTime; // last time we received a packet
                                           // with measurements
    uint64_t RxTotalBytes, RxPrevMeasuredTotalBytes, TxTotalBytes,
        TxPrevMeasuredTotalBytes;
    uint64_t EstimatedFreeTime; // to schedule packets: estimated time when the
                                // link will be done sending its packets
                                // according to the rate
    struct sockaddr_in Remote;  // where to send to
    int SocketFd;              // local socket descriptor
    uint16_t RemoteLinkIdx;     // index of this link as the other side sees it
    pthread_t ThreadMeasurements;
    uint8_t ProbationModeCounter;
  } * Link;

  uint16_t LocalCommDeviceNumber;
  uint16_t RemoteCommDeviceNumber;
  uint16_t TotalLinkNumber; // =multipe of LocalCommDeviceNumber and
                            // RemoteCommDeviceNumber, should be less than
                            // 0xFFFF as 0xFFFF is used in the handshake
  struct timeval t0;
  pthread_t *ThreadRxSocket; // each socket reader is a separate thread
  pthread_t ThreadHandshake, ThreadRedundant, ThreadScheduler,
      ThreadDecoder;
  size_t MaxUdpBufferSize = 3000;
  char **UdpBuffer;
  uint64_t MeasureInterval = 100000; // 100 miliseconds
  uint64_t MinLinkTXRate = 102400;    // 100KBps
  uint64_t MaxLinkTXRate = 10240000;  // 10000KBps
  uint16_t CrsRateNumerator, CrsRateDenominator;

  uint8_t *EncoderBlocks[256];
  uint8_t *RecoverSrcs[256];
  uint8_t *RecoverOutp[256];
  uint8_t ErrList[256];
  // Coefficient matrices
  uint8_t *EncodeMatrix2, *EncodeMatrix, *DecodeMatrix;
  uint8_t *InvertMatrix, *TempMatrix;
  uint8_t *GTbls;
  uint8_t *GTblsd;
  uint8_t DecodeIndex[256];

  uint16_t SendSequenceId, MaxSendSequenceId;
  uint8_t *SendSequenceDataBuffer, *SendRedundantSequenceDataBuffer,
      *DecodedSequenceBuffer;
  uint16_t PacketSizeMin = 500;
  uint16_t PacketSizeMax = 1500;
  uint8_t *SendPacketDataBuffer, *SendRedundantPacketDataBuffer,
      *SendPacketSchedulerBuffer, *ReceivePacketDataBuffer;
  int SendSequencePipe[2], SendPacketPipe[2], ReceivePacketPipe[2],
      ReceiveSequencePipe[2]; // pipes to exchange data between threads
  int SendSequencePipeSize, SendPacketPipeSize,
      ReceiveSequencePipeSize;
  pthread_mutex_t SendPacketWriterMtx, ReceivePacketWriterMtx, SendMtx,
      ReceiveMtx;
  uint64_t CorrectSequences, CorrectSequencesDropped,
      IncorrectSequences; // statistics only. Can be removed.

  struct DecoderSequenceT {
    uint16_t OriginalBlockNumber, TotalBlockNumber;
    uint16_t AvailableBlockNumber;
    uint16_t PacketSize;
    bool Forbidden; // this flag is set to drop packets for recently decoded
                    // sequence
    uint8_t *Packets;
  } * DecoderSequences;

  void sendRate(uint16_t LinkIdx, uint16_t Rate);
  void sentPacketToLink(uint16_t PktSize, char *PktBuf, uint16_t LinkIdx);
  uint16_t findRemoteIndexByAddr(uint16_t LocalSocketIdx,
                                 struct sockaddr_in SenderAddr);

  // methods below that have names starting with "run" are threads. They are
  // started with static helpers

  struct HelperT {
    MultiLink *Ml;
    uint16_t Num;
  } * HelperStruct;

  static void *helperSockets(void *Arg) {
    uint16_t Value;
    struct HelperT *HtPtr;
    HtPtr = (struct HelperT *)Arg;
    MultiLink *ML = reinterpret_cast<MultiLink *>(HtPtr->Ml);
    Value = HtPtr->Num;
    free(HtPtr);
    ML->runSockets(Value);
    return 0;
  }
  void runSockets(uint16_t SocketIdx);

  static void *helperMeasurements(void *Arg) {
    uint16_t Value;
    struct HelperT *HtPtr;
    HtPtr = (struct HelperT *)Arg;
    MultiLink *ML = reinterpret_cast<MultiLink *>(HtPtr->Ml);
    Value = HtPtr->Num;
    // free(htPtr);
    ML->runMeasurements(Value);
    return 0;
  }
  void runMeasurements(uint16_t LinkIdx);

  static void *helperHandshake(void *Arg) {
    MultiLink *ML = reinterpret_cast<MultiLink *>(Arg);
    ML->runHandshake();
    return 0;
  }
  void runHandshake();

  static void *helperRedundant(void *Arg) {
    MultiLink *ML = reinterpret_cast<MultiLink *>(Arg);
    ML->runMakeRedundantPackets();
    return 0;
  }
  void runMakeRedundantPackets();

  static void *helperScheduler(void *Arg) {
    MultiLink *ML = reinterpret_cast<MultiLink *>(Arg);
    ML->runScheduleAndSendPacket();
    return 0;
  }
  void runScheduleAndSendPacket();

  static void *helperDecoder(void *Arg) {
    MultiLink *ML = reinterpret_cast<MultiLink *>(Arg);
    ML->runDecoder();
    return 0;
  }
  void runDecoder();

  /**
   * Sets local interface and and port to listen at/transmit from.
   * Has to be called for each local interface/port pair.
   * Note: because it binds interfaces, it requires super user privilegies.
   * @param Index is the index of a local communication device to set
   * parameters, i.e. 0, 1, 2 ... (n_local-1), where n is the number of local
   * devides set by setCommDeviceNumber()
   * @param Name is the name of the network intefface to bind to, i.e. "eth0"
   * @param Port is the port to bind to. This parameter can be 0 for connection
   * initialization party, so the operating system assigns a random port.
   */
  void setLocalIfaceAndPort(uint16_t Index, char *Name, uint16_t Port);

  /**
   * Sets remote IP address and port to connect to.
   * Has to be called for each remote pair of remote address:port
   * This function needs to be called by connection initiator party, i.e. a
   * party that is will call initiateLinks()
   * @param Num is is the index of a remote communication device, i.e. 0, 1, 2
   * ... (n_remote-1), where n is the number of local devides set by
   * setCommDeviceNumber()
   * @param SzIPaddr is the IP address or the hostname of the remote side
   * @param Port is the port of the remote side
   */
  void setRemoteAddrAndPort(uint16_t Num, char *SzIPaddr, uint16_t Port);
};

#endif
