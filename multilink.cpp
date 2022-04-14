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

#include "multilink.h"
#include <isa-l.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#undef DEBUG_PRINT
#ifdef LLMLNCL_DEBUG_PRINT
#define DEBUG_PRINT 1
#endif

#define ALLOC_CHECK(Ptr, Name) if (!(Ptr)) { \
                                 printf("Error allocating memory for "); \
                                 printf((Name)); \
                                 printf("\n"); \
                                 exit(-1); \
                               }
#define READ_CHECK(FD, Buf, Cnt) if (read((FD), (Buf), (Cnt)) != (Cnt)) { \
                                 printf("Error reading %d bytes\n", \
                                        (int32_t)Cnt); \
                                 exit(-1); \
                               }
#define WRITE_CHECK(FD, Buf, Cnt) if (write((FD), (Buf), (Cnt)) != (Cnt)) { \
                                 printf("Error writing %d bytes\n", \
                                        (int32_t)Cnt); \
                                 exit(-1); \
                               }

#define PIPE_CHECK(Pp) if (pipe(Pp) != 0) { \
                         printf("Error opening pipe\n"); \
                         exit(-1); \
                       }

MultiLink::MultiLink() {
  TotalLinkNumber = 0;
  LocalCommDeviceNumber = 0;
  RemoteCommDeviceNumber = 0;
  TopLocalLink = 0; // Number of the latest link set up
  TopRemoteLink = 0; // Number of the latest link set up
}

void MultiLink::setCommDeviceNumber(uint16_t NumLocal, uint16_t NumRemote) {
  if (NumLocal > 32 || NumRemote > 32) {
    printf("The library do not support local/remote links greater than 32\n");
    exit(-1);
  }
  LocalCommDeviceNumber = NumLocal;
  RemoteCommDeviceNumber = NumRemote;
  TotalLinkNumber = LocalCommDeviceNumber * RemoteCommDeviceNumber;
  Link = (struct LinkT *)malloc(sizeof(struct LinkT) * TotalLinkNumber);
  ALLOC_CHECK(Link, "links");
  for (int32_t i = 0; i < TotalLinkNumber; i++) {
    Link[i].Status = 0; // set to inactive
    Link[i].Remote.sin_addr.s_addr = 0;
    Link[i].RxTotalBytes = 0;
    Link[i].RxPrevMeasuredTotalBytes = 0;
    Link[i].TxTotalBytes = 0;
    Link[i].TxPrevMeasuredTotalBytes = 0;
    Link[i].TxDesiredRate = (uint64_t)1048576; // default = 1MBps
    Link[i].ProbationModeCounter = 0;
    Link[i].RemoteLinkIdx = 0xFFFF; // forbidden index;
    for (int32_t k = 0; k < 10; k++) {      // reset all rates
      Link[i].RxRateStatistics[k] = 0;
      Link[i].TxRateStatistics[k] = 0;
    }
    Link[i].EstimatedFreeTime = getLocalTime();
  }
  // to address Links: Link [ local_index * RemoteCommDeviceNumber +
  // remote_index]

  ThreadRxSocket =
      (pthread_t *)malloc(sizeof(pthread_t) * LocalCommDeviceNumber);
  ALLOC_CHECK(ThreadRxSocket, "socket threads");
  for (int32_t i = 0; i < LocalCommDeviceNumber; i++) {
    ThreadRxSocket[i] = 0;
  }
  UdpBuffer = (char **)malloc(sizeof(char *) * LocalCommDeviceNumber);
  ALLOC_CHECK(UdpBuffer, "udp buffer");
  for (int32_t i = 0; i < LocalCommDeviceNumber; i++) {
    UdpBuffer[i] = (char *)malloc(sizeof(char) * MaxUdpBufferSize);
    ALLOC_CHECK(UdpBuffer[i], "udp buffer");
  }
  // redundant packet generation parameters
  CrsRateNumerator = 4;   // default
  CrsRateDenominator = 9; // default
  MaxSendSequenceId = 8192;
  SendSequenceId = 0;

  EncodeMatrix = (uint8_t *)malloc(65536);
  ALLOC_CHECK(EncodeMatrix, "RS Encode Matrix");
  EncodeMatrix2 = (uint8_t *)malloc(65536);
  ALLOC_CHECK(EncodeMatrix2, "RS Encode Matrix");
  DecodeMatrix = (uint8_t *)malloc(65536);
  ALLOC_CHECK(DecodeMatrix, "RS Decode Matrix");
  InvertMatrix = (uint8_t *)malloc(65536);
  ALLOC_CHECK(InvertMatrix, "RS Invert Matrix");
  TempMatrix =(uint8_t *) malloc(65536);
  ALLOC_CHECK(TempMatrix, "RS Temp Matrix");
  GTbls = (uint8_t *)malloc(65536 * 32);
  ALLOC_CHECK(GTbls, "RS Galua Tables");
  GTblsd = (uint8_t *)malloc(65536 * 32);
  ALLOC_CHECK(GTblsd, "RS Galua Tables");
  for (int32_t i = 0; i < 256; i++) {
    RecoverOutp[i] = (uint8_t *)malloc(65536);
  }

  // open inter-thread pipes, allocate memory buffers and start threads:

  pthread_mutex_init(&SendMtx, NULL);
  pthread_mutex_init(&ReceiveMtx, NULL);

  PIPE_CHECK(SendSequencePipe);
  fcntl(SendSequencePipe[0], F_SETPIPE_SZ, 1024 * 256);
  SendSequencePipeSize = fcntl(SendSequencePipe[0], F_GETPIPE_SZ);
  SendSequenceDataBuffer = (uint8_t *)malloc(256 * PacketSizeMax + 6);
  ALLOC_CHECK(SendSequenceDataBuffer, "send sequence buffer");
  SendPacketDataBuffer = (uint8_t *)malloc(PacketSizeMax);
  ALLOC_CHECK(SendPacketDataBuffer, "send packet buffer");
  pthread_create(&ThreadRedundant, NULL, helperRedundant,
                 this); // redundant packet generaton thread

  PIPE_CHECK(SendPacketPipe);
  fcntl(SendPacketPipe[0], F_SETPIPE_SZ, 1024 * 256);
  SendPacketPipeSize = fcntl(SendPacketPipe[0], F_GETPIPE_SZ);
  SendRedundantSequenceDataBuffer =
      (uint8_t *)malloc(256 * PacketSizeMax + 6);
  ALLOC_CHECK(SendRedundantSequenceDataBuffer, "redundant sequence buffer");
  SendRedundantPacketDataBuffer = (uint8_t *)malloc(PacketSizeMax);
  ALLOC_CHECK(SendRedundantPacketDataBuffer, "redundant packet buffer");
  SendPacketSchedulerBuffer = (uint8_t *)malloc(PacketSizeMax);
  ALLOC_CHECK(SendPacketSchedulerBuffer, "scheduler packet buffer");
  pthread_mutex_init(&SendPacketWriterMtx, NULL);
  pthread_create(&ThreadScheduler, NULL, helperScheduler,
                 this); // UDP packet sender thread

  PIPE_CHECK(ReceivePacketPipe);
  // to send data from socket receiver to sequence decoder
  fcntl(ReceivePacketPipe[0], F_SETPIPE_SZ, 1024 * 256);
  PIPE_CHECK(ReceiveSequencePipe);
  // to send data from sequence decoder to the final recepient
  fcntl(ReceiveSequencePipe[0], F_SETPIPE_SZ, 1024 * 256);
  ReceiveSequencePipeSize = fcntl(ReceiveSequencePipe[0], F_GETPIPE_SZ);
  ReceivePacketDataBuffer = (uint8_t *)malloc(PacketSizeMax);
  ALLOC_CHECK(ReceivePacketDataBuffer, "receive packet buffer");
  DecoderSequences = (struct DecoderSequenceT *)malloc(
      MaxSendSequenceId * sizeof(struct DecoderSequenceT));
  ALLOC_CHECK(DecoderSequences, "decoder sequences");
  DecodedSequenceBuffer = (uint8_t *)malloc(256 * PacketSizeMax + 6);
  ALLOC_CHECK(DecodedSequenceBuffer, "decoder buffer");
  pthread_mutex_init(&ReceivePacketWriterMtx, NULL);
  pthread_create(&ThreadDecoder, NULL, helperDecoder,
                 this); // sequence decoder thread
}

MultiLink::~MultiLink() {
  void *SomePtr;

  if (TotalLinkNumber > 0) { // only if there was initialization before

    // stop handshake thread if there was an active link
    for (int32_t i = 0; i < TotalLinkNumber; i++) {
      if (Link[i].Status > 0)  {
        pthread_cancel(ThreadHandshake);
        pthread_join(ThreadHandshake, &SomePtr);
        break;
      }
    }
    // stop redundant packet generation thread and free memories
    pthread_cancel(ThreadRedundant);
    pthread_join(ThreadRedundant, &SomePtr);
    close(SendSequencePipe[1]);
    close(SendSequencePipe[0]);
    free(SendPacketDataBuffer);
    free(SendSequenceDataBuffer);

    // stop packet sender thread and free memories
    pthread_cancel(ThreadScheduler);
    pthread_join(ThreadScheduler, &SomePtr);
    close(SendPacketPipe[1]);
    close(SendPacketPipe[0]);
    free(SendPacketSchedulerBuffer);
    free(SendRedundantPacketDataBuffer);
    free(SendRedundantSequenceDataBuffer);

    // stop measurements/rate control
    for (int32_t i = 0; i < TotalLinkNumber; i++)
      if (Link[i].Status == 3) {
        pthread_cancel(Link[i].ThreadMeasurements);
        pthread_join(Link[i].ThreadMeasurements, &SomePtr);
      }

    // stop receivers amd free receive buffers
    for (int32_t i = 0; i < LocalCommDeviceNumber; i++) {
      if (ThreadRxSocket[i]) {
        pthread_cancel(ThreadRxSocket[i]);
        pthread_join(ThreadRxSocket[i], &SomePtr);
      }
      free(UdpBuffer[i]);
    }

    // stop decoder and free memories
    pthread_cancel(ThreadDecoder);
    pthread_join(ThreadDecoder, &SomePtr);
    close(ReceivePacketPipe[1]);
    close(ReceivePacketPipe[0]);
    close(ReceiveSequencePipe[1]);
    close(ReceiveSequencePipe[0]);
    free(ReceivePacketDataBuffer);
    for (int32_t i = 0; i < MaxSendSequenceId; i++)
      if (DecoderSequences[i].AvailableBlockNumber > 0)
        free(DecoderSequences[i].Packets);
    free(DecoderSequences);
  }
}

void MultiLink::setRemoteAddrAndPort(uint16_t Num, char *SzIPaddr,
                                     uint16_t Port) {
  if (inet_pton(
          AF_INET, SzIPaddr,
          &Link[0 * RemoteCommDeviceNumber + Num].Remote.sin_addr) <= 0) {
    // error
    printf("error parsing remote hostname %s\n", SzIPaddr);
    exit(-1);
  }
  for (int32_t ILocal = 0; ILocal < LocalCommDeviceNumber; ILocal++) {
    // replicate to all
    Link[ILocal * RemoteCommDeviceNumber + Num].Remote.sin_addr.s_addr =
        Link[0 * RemoteCommDeviceNumber + Num].Remote.sin_addr.s_addr;
    Link[ILocal * RemoteCommDeviceNumber + Num].Remote.sin_family =
        AF_INET;
    Link[ILocal * RemoteCommDeviceNumber + Num].Remote.sin_port =
        htons(Port);
  }
}

void MultiLink::addRemoteAddrAndPort(char *SzIPaddr, uint16_t Port) {
  setRemoteAddrAndPort(TopRemoteLink, SzIPaddr, Port);
  TopRemoteLink++;
  if (TopRemoteLink > RemoteCommDeviceNumber) {
    printf("Operation exceed number of allowed remote Links (%u)\n",
           RemoteCommDeviceNumber);
    exit(-1);
  }
}

void MultiLink::setLocalIfaceAndPort(uint16_t ILocal, char *Name,
                                     uint16_t Port) {
  uint16_t IRemote;
  int32_t SocketFd, ErrorCode;
  struct sockaddr_in Local;
  struct HelperT *Ht1;
  SocketFd = socket(AF_INET, SOCK_DGRAM, 0);
  if (SocketFd < 0) {
    printf("error creating local socket at iface #%u\n", ILocal);
    exit(-1);
  }
  if (RemoteCommDeviceNumber <= 0) {
    printf("error, numbur of remote device can't be 0 or less\n");
    exit(-1);
  }
  for (IRemote = 0; IRemote < RemoteCommDeviceNumber; IRemote++) {
    // replicate to all
    Link[ILocal * RemoteCommDeviceNumber + IRemote].SocketFd =
        SocketFd;
  }
  Local.sin_family = AF_INET;
  Local.sin_port = htons(Port);
  Local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(SocketFd, (const struct sockaddr *)&Local, sizeof(Local)) < 0) {
    printf("error binding local socket to port at iface #%u\n", ILocal);
    exit(-1);
  }
  if ((ErrorCode = setsockopt(SocketFd, SOL_SOCKET, SO_BINDTODEVICE, Name,
                               strlen(Name))) < 0) {
    printf("error binding local socket at iface #%u to device %s\n", ILocal,
           Name);
    exit(-1);
  }

  Ht1 = (struct HelperT *)malloc(sizeof(struct HelperT));
  ALLOC_CHECK(Ht1, "Helper thread 1");
  Ht1->Ml = this;
  Ht1->Num = ILocal;
  pthread_create(&(ThreadRxSocket[ILocal]), NULL, helperSockets, Ht1);
}

void MultiLink::addLocalIfaceAndPort(char *Name, uint16_t Port) {
  setLocalIfaceAndPort(TopLocalLink, Name, Port);
  TopLocalLink++;
  if (TopLocalLink > LocalCommDeviceNumber) {
    printf("Operation exceed number of allowed remote Links (%u)\n",
           LocalCommDeviceNumber);
    exit(-1);
  }
}

void MultiLink::runSockets(uint16_t LocalSocketIdx) {
  uint16_t i, ui16value, RemoteIdx;
  struct sockaddr_in SenderAddr;
  socklen_t AddrLen;
  size_t MsgLen;

  while (1) {
    // receive a packet
    AddrLen = sizeof(struct sockaddr_in);
    MsgLen = recvfrom(
        Link[LocalSocketIdx * RemoteCommDeviceNumber].SocketFd,
        UdpBuffer[LocalSocketIdx], MaxUdpBufferSize, MSG_WAITALL,
        (struct sockaddr *)&SenderAddr, &AddrLen);
    RemoteIdx = findRemoteIndexByAddr(LocalSocketIdx, SenderAddr);
    if (LocalSocketIdx * RemoteCommDeviceNumber + RemoteIdx < TotalLinkNumber)
      Link[LocalSocketIdx * RemoteCommDeviceNumber + RemoteIdx]
          .RxTotalBytes += MsgLen;

#ifdef DEBUG_PRINT
    printf("new packet at local comm#%d: len=%d, from %s:%d\n",
           LocalSocketIdx, MsgLen, inet_ntoa(SenderAddr.sin_addr),
           ntohs(SenderAddr.sin_port));
    if (MsgLen > 1)
      printf("fst byte = %0x\n",
             *((uint16_t *)&UdpBuffer[LocalSocketIdx][0]));
#endif

    // process a packet
    switch (MsgLen) {

    case 2: // handshake
      ui16value = *((uint16_t *)&UdpBuffer[LocalSocketIdx][0]);
      if (ui16value != 0xFFFF) { // hanshake request
        // check if we already have this sender address in the Links
        if (RemoteIdx == RemoteCommDeviceNumber) {
          // new request: find an available link ID
          for (i = 0; i < RemoteCommDeviceNumber; i++) {
            if (Link[LocalSocketIdx * RemoteCommDeviceNumber + i]
                    .Status == 0)
              break;
          }
          if (i < RemoteCommDeviceNumber) {
            // we found an empty link slot, so save source IP:port, save remote
            // ID and send our link ID to continue
            Link[LocalSocketIdx * RemoteCommDeviceNumber + i]
                .Remote.sin_addr.s_addr = SenderAddr.sin_addr.s_addr;
            Link[LocalSocketIdx * RemoteCommDeviceNumber + i]
                .Remote.sin_port = SenderAddr.sin_port;
            Link[LocalSocketIdx * RemoteCommDeviceNumber + i]
                .Remote.sin_family = AF_INET;
            Link[LocalSocketIdx * RemoteCommDeviceNumber + i].Status = 2;
            // handshake in progress 2
            Link[LocalSocketIdx * RemoteCommDeviceNumber + i].RemoteLinkIdx
                = ui16value;
            // the value that the other party
            // sent is the link idx
          } else {
            // no free slots, error?
            break;
          }
        } else {
          // we already have this address, check for Status
          if (Link[LocalSocketIdx * RemoteCommDeviceNumber + RemoteIdx]
                  .Status == 2) {
            // handshake started, but the other party SendS the handshate again:
            // they did not get our reply. Resend:
            i = RemoteIdx;
          } else {
            if (Link[LocalSocketIdx * RemoteCommDeviceNumber +
                     RemoteIdx]
                    .Status == 1) {
              // we were the source of this initialization, now we have a reply,
              // so switch to the next stage
              Link[LocalSocketIdx * RemoteCommDeviceNumber + RemoteIdx]
                  .Status = 2;
              Link[LocalSocketIdx * RemoteCommDeviceNumber + RemoteIdx]
                  .RemoteLinkIdx = ui16value; // the value that the other party
                                              // sent is the link idx
            } else {
              // protocol error? hope it will never happen
            }
            break; // we dont have to send the reply back as we have another
                   // thread that will send 0xFFFF
          }
        }
        // send our link Idx to the remote
        ui16value = LocalSocketIdx * RemoteCommDeviceNumber + i;
        sendto(
            Link[LocalSocketIdx * RemoteCommDeviceNumber + i].SocketFd,
            (char *)(&ui16value), 2, MSG_DONTWAIT,
            (const struct sockaddr *)&SenderAddr, sizeof(SenderAddr));
        Link[LocalSocketIdx * RemoteCommDeviceNumber + i]
            .TxTotalBytes += 2; // increment tx byte couter
      } else {                  // handshake confirmation
        // check if we have this sender address in the links
        if (RemoteIdx < RemoteCommDeviceNumber) {
          if (Link[LocalSocketIdx * RemoteCommDeviceNumber + RemoteIdx]
                  .Status == 2) {
            struct HelperT *Ht2;
            // handshake started and processed correctly. Complete it now
            ui16value = 0xFFFF;
            sendto(Link[LocalSocketIdx * RemoteCommDeviceNumber +
                        RemoteIdx]
                       .SocketFd,
                   (char *)(&ui16value), 2, MSG_DONTWAIT,
                   (const struct sockaddr *)&SenderAddr, sizeof(SenderAddr));
            Link[LocalSocketIdx * RemoteCommDeviceNumber + RemoteIdx]
                .TxTotalBytes += 2; // increment tx byte couter
            Link[LocalSocketIdx * RemoteCommDeviceNumber + RemoteIdx]
                .Status = 3; // handshake completed
            // start measurement thread
            Ht2 = (struct HelperT *)malloc(sizeof(struct HelperT));
            ALLOC_CHECK(Ht2, "Helper thread 2");
            Ht2->Ml = this;
            Ht2->Num =
                LocalSocketIdx * RemoteCommDeviceNumber + RemoteIdx;
            pthread_create(
                &(Link[LocalSocketIdx * RemoteCommDeviceNumber +
                       RemoteIdx]
                      .ThreadMeasurements),
                NULL, helperMeasurements, Ht2);
#ifdef DEBUG_PRINT
            printf("handshake over link %u completed\n",
                   LocalSocketIdx * RemoteCommDeviceNumber + RemoteIdx);
#endif
          }
        }
      }
      break;

    case 4: // rate control message
      RemoteIdx = *((uint16_t *)&UdpBuffer[LocalSocketIdx][0]);
      // this is the link id as remote sees it
      ui16value = *((uint16_t *)&UdpBuffer[LocalSocketIdx][2]);
      // this is the rate in Kb/s
      if (RemoteIdx < RemoteCommDeviceNumber) {
        for (i = 0; i < TotalLinkNumber; i++) {
          // we need to find a link with remote index ID == i
          if (Link[i].RemoteLinkIdx == RemoteIdx) { // found it!
            Link[i].RxReportedRate = ui16value;
            Link[i].LastMeasurementsReceivedTime = getLocalTime();
          }
        }
      }
      break;

    default: // forward packet to the decoder to be processed
      if (MsgLen > 5) {
        pthread_mutex_lock(&ReceivePacketWriterMtx);
        WRITE_CHECK(ReceivePacketPipe[1], &MsgLen, 2);
        WRITE_CHECK(ReceivePacketPipe[1], UdpBuffer[LocalSocketIdx], MsgLen);
        pthread_mutex_unlock(&ReceivePacketWriterMtx);
      }
    }
  }
}

void MultiLink::initiateLinks() {
  uint16_t LinkIdx;
  if (TopLocalLink != LocalCommDeviceNumber ||
      TopRemoteLink != RemoteCommDeviceNumber) {
    printf("Local or remote links set up does not equal to requested:\n");
    printf("Local set %u, requested %u\n", TopLocalLink, LocalCommDeviceNumber);
    printf("Remote set %u, requested %u\n", TopRemoteLink,
           RemoteCommDeviceNumber);
    exit(-1);
  }

  for (LinkIdx = 0; LinkIdx < TotalLinkNumber; LinkIdx++)
    Link[LinkIdx].Status = 1; // set to handshake init
  pthread_create(&ThreadHandshake, NULL, helperHandshake, this);
}

void MultiLink::runHandshake() {

  /*
  Handshake algorithm:
  1. initializer SendS its ID to the remote (id should be < then 0xFFFF)
  2. Remote replies with its ID
  3. initlializer replies with 0xFFFF to confirm/complete ()
  4. If remote receives 0xFFFF, it replies with 0xFFFF to confirm completion
  */

  uint16_t LinkIdx, Id;
  while (1) {
    // go over all links
    for (LinkIdx = 0; LinkIdx < TotalLinkNumber; LinkIdx++) {
      switch (Link[LinkIdx].Status) {
      case 1: // send Link ID to init handshake
        Id = LinkIdx;
        sendto(Link[LinkIdx].SocketFd, (char *)&Id, 2, MSG_DONTWAIT,
               (const struct sockaddr *)&Link[LinkIdx].Remote,
               sizeof(sockaddr_in));
        Link[LinkIdx].TxTotalBytes += 2; // increment tx byte couter

        break;

      case 2: // send 0xFFFF to complete the handshake
        Id = 0xFFFF;
        sendto(Link[LinkIdx].SocketFd, (char *)&Id, 2, MSG_DONTWAIT,
               (const struct sockaddr *)&Link[LinkIdx].Remote,
               sizeof(sockaddr_in));
        Link[LinkIdx].TxTotalBytes += 2; // increment tx byte couter
        break;
      }
    }
    sleep(1);
  }
}

void MultiLink::runMeasurements(uint16_t LinkIdx) {
  uint64_t CurrentTime, CurTotalBytes, SomeRateObserved;
  uint16_t i;
  bool RateIncreaseObserved, RateDecreaseObserved;
  float AverageOver4Last, MaxAverageOver4Last;
  RateDecreaseObserved = false;
  RateIncreaseObserved = false;
  SomeRateObserved = 1;
#ifdef DEBUG_PRINT
  printf("measurements over link %u started\n", LinkIdx);
#endif
  Link[LinkIdx].LastRateControlActionTime = getLocalTime();
  MaxAverageOver4Last = 0;

  while (1) {
    usleep(MeasureInterval);
    CurrentTime = getLocalTime();

    // RX measurements: compute current rate
    CurTotalBytes = Link[LinkIdx].RxTotalBytes;
    Link[LinkIdx].RxMeasuredRate =
        1.0e6 / MeasureInterval *
        (CurTotalBytes -
         Link[LinkIdx].RxPrevMeasuredTotalBytes); // bytes per second
    Link[LinkIdx].RxPrevMeasuredTotalBytes = CurTotalBytes;
    // send measurerements to tx
    sendRate(LinkIdx,
             (uint16_t)(Link[LinkIdx].RxMeasuredRate / 1024)); // in KBps

    // TX measurements: compute current rate
    CurTotalBytes = Link[LinkIdx].TxTotalBytes;
    Link[LinkIdx].TxMeasuredRate =
        1.0e6 / MeasureInterval *
        (CurTotalBytes -
         Link[LinkIdx].TxPrevMeasuredTotalBytes); // bytes per second
    Link[LinkIdx].TxPrevMeasuredTotalBytes = CurTotalBytes;

    // statistics update
    if ((Link[LinkIdx].LastMeasurementsReceivedTime + 2 * MeasureInterval) <
        CurrentTime) { // we did not receive measurements during 2 last
                       // intervals
      Link[LinkIdx].RxReportedRate = 0;
    }
    // shift all previous statistics by one
    for (i = 9; i > 0; i--) {
      Link[LinkIdx].RxRateStatistics[i] =
          Link[LinkIdx].RxRateStatistics[i - 1];
      Link[LinkIdx].TxRateStatistics[i] =
          Link[LinkIdx].TxRateStatistics[i - 1];
    }
    Link[LinkIdx].RxRateStatistics[0] =
        (float)Link[LinkIdx].RxReportedRate;
    Link[LinkIdx].TxRateStatistics[0] =
        (float)(Link[LinkIdx].TxMeasuredRate / 1024); // convert to KBps

    // compute average reported rate
    AverageOver4Last = 0;
    for (i = 0; i < 4; i++)
      AverageOver4Last += Link[LinkIdx].RxRateStatistics[i];
    AverageOver4Last *= 0.25;
    if (AverageOver4Last > MaxAverageOver4Last)
      MaxAverageOver4Last = AverageOver4Last;

#ifdef DEBUG_PRINT
    // print measured stats
    printf("Link %2u, meas_rx=%5.0fK, rep_rx=%5.0lfK, meas_tx=%5.0fK, "
           "desi_tx=%5lu\n", LinkIdx, Link[LinkIdx].RxMeasuredRate / 1024.0,
    Link[LinkIdx].RxRateStatistics[0],
    Link[LinkIdx].TxRateStatistics[0],
    Link[LinkIdx].TxDesiredRate >> 10);
#endif

    // Rate Control section

    if (Link[LinkIdx].ProbationModeCounter > 0) {
      // In Probation mode
      if (Link[LinkIdx].RxRateStatistics[0] < MinLinkTXRate) {
          Link[LinkIdx].ProbationModeCounter = 0;
#ifdef DEBUG_PRINT
          printf("Probation is skipped%d\n");
#endif
          continue;
      }
      // first time run only. Reset counters
      if (Link[LinkIdx].ProbationModeCounter == 8) {
        RateDecreaseObserved = false;
        RateIncreaseObserved = false;
      }
      // one half of probation time reached. Change TX rate.
      if (Link[LinkIdx].ProbationModeCounter == 5) {
        Link[LinkIdx].TxDesiredRate -=
            2 * (Link[LinkIdx].TxDesiredRate -
                Link[LinkIdx].TxRateBeforeProbation);
        if (Link[LinkIdx].TxDesiredRate < MinLinkTXRate) {
            Link[LinkIdx].TxDesiredRate = MinLinkTXRate;
        } else if (Link[LinkIdx].TxDesiredRate > MaxLinkTXRate) {
            Link[LinkIdx].TxDesiredRate = MaxLinkTXRate;
        }
      }

      // update probation flags
      if ((Link[LinkIdx].RxRateStatistics[0] +
           Link[LinkIdx].RxRateStatistics[1] +
           Link[LinkIdx].RxRateStatistics[2] +
           Link[LinkIdx].RxRateStatistics[3]) *
              0.95 >
          (Link[LinkIdx].RxRateStatistics[4] +
           Link[LinkIdx].RxRateStatistics[5] +
           Link[LinkIdx].RxRateStatistics[6] +
           Link[LinkIdx].RxRateStatistics[7])) {
        // 4 last numbers are REALLY greater than 4 numbers before
        RateIncreaseObserved = true;
        SomeRateObserved = (uint64_t)(AverageOver4Last * 1024.0);
        if (SomeRateObserved < MinLinkTXRate) {
            SomeRateObserved = MinLinkTXRate;
        }
        else if (SomeRateObserved > MaxLinkTXRate) {
            SomeRateObserved = MaxLinkTXRate;
        }

      }
      if ((Link[LinkIdx].RxRateStatistics[1] +
           Link[LinkIdx].RxRateStatistics[2]) /
              2 <
          (Link[LinkIdx].RxRateStatistics[3] +
           Link[LinkIdx].RxRateStatistics[4] +
           Link[LinkIdx].RxRateStatistics[5]) /
              3 * 0.95) {
        // an average of two prevoius numbers is REALLY less than an average of
        // three numbers before
        RateDecreaseObserved = true;
      }

      if (Link[LinkIdx].ProbationModeCounter ==
          1) { // this is the last time. Need to decide
#ifdef DEBUG_PRINT
        printf("inc=%u, dec=%u ", RateIncreaseObserved, RateDecreaseObserved);
#endif
        if (RateIncreaseObserved && RateDecreaseObserved) {
          Link[LinkIdx].TxDesiredRate =
              SomeRateObserved; // probation successful
#ifdef DEBUG_PRINT
          printf("link %u: probation successful. Chahge to %lu\n", LinkIdx,
                 Link[LinkIdx].TxDesiredRate);
#endif
        } else {
          Link[LinkIdx].TxDesiredRate =
              Link[LinkIdx].TxRateBeforeProbation; // probation failed.
                                                       // Return original rate
#ifdef DEBUG_PRINT
          printf("link %u: probation failed. Back to %lu \n", LinkIdx,
                 Link[LinkIdx].TxDesiredRate);
#endif
        }
        Link[LinkIdx].LastRateControlActionTime = CurrentTime;
      }
      Link[LinkIdx].ProbationModeCounter--;
    } else { // Not in Probation mode

      if ((Link[LinkIdx].TxRateStatistics[0] < (MinLinkTXRate / 1024)) ||
          (Link[LinkIdx].TxRateStatistics[1] < (MinLinkTXRate / 1024)) ||
          (Link[LinkIdx].TxRateStatistics[2] < (MinLinkTXRate / 1024)))
         continue; // we sent nothing
      // data is sending, but no replies from the rx in 3 intervals
      if (Link[LinkIdx].RxRateStatistics[0] +
              Link[LinkIdx].RxRateStatistics[1] +
              Link[LinkIdx].RxRateStatistics[2] == 0) {
        Link[LinkIdx].TxDesiredRate = MinLinkTXRate; // set to 100 KBps;
        Link[LinkIdx].LastRateControlActionTime = CurrentTime;
#ifdef DEBUG_PRINT
        printf("link %u, rate decrease to 100 as zero feedback %lu\n",
               LinkIdx, Link[LinkIdx].TxDesiredRate);
#endif
        continue;
      }
      if ((Link[LinkIdx].LastRateControlActionTime + MeasureInterval * 5) >
          CurrentTime)
        continue; // not enough time since last action

      if (Link[LinkIdx].RxRateStatistics[0] <
          0.95 * Link[LinkIdx].TxRateStatistics[0]) {
        // rate is lower than expected, need to drop the rate

        if (1.05 * AverageOver4Last <
            0.25 * (Link[LinkIdx].TxRateStatistics[1] +
                    Link[LinkIdx].TxRateStatistics[2] +
                    Link[LinkIdx].TxRateStatistics[3] +
                    Link[LinkIdx].TxRateStatistics[4])) {
          // rate is just lower for a long time
          Link[LinkIdx].TxDesiredRate =
              (uint64_t)(0.9 * 1024 * AverageOver4Last); // 0.9 is to flush
                                                         // possible Packets
                                                         // that are in the link
          if (Link[LinkIdx].TxDesiredRate < MinLinkTXRate)
            Link[LinkIdx].TxDesiredRate = MinLinkTXRate; // min threshold
          Link[LinkIdx].LastRateControlActionTime = CurrentTime;
#ifdef DEBUG_PRINT
          printf("Link %u, rate decrease to %lu\n", LinkIdx,
                 Link[LinkIdx].TxDesiredRate);
#endif
        }
        continue;
      }

      // no changes during last 15 measurements
      if ((Link[LinkIdx].LastRateControlActionTime + MeasureInterval * 15) <
          CurrentTime) {
        // check if other links are in probation
        for (i = 0; i < TotalLinkNumber; i++)
          if (Link[i].ProbationModeCounter > 0)
            break;
        if (i == TotalLinkNumber) {
          // probe a higher rate
          Link[LinkIdx].ProbationModeCounter = 8;
          Link[LinkIdx].TxRateBeforeProbation =
              Link[LinkIdx].TxDesiredRate;
          Link[LinkIdx].TxDesiredRate =
              (uint64_t)(1.2 * Link[LinkIdx].TxDesiredRate);
          if (Link[LinkIdx].TxDesiredRate < MinLinkTXRate) {
              Link[LinkIdx].TxDesiredRate = MinLinkTXRate;
         } else if (Link[LinkIdx].TxDesiredRate > MaxLinkTXRate) {
             Link[LinkIdx].TxDesiredRate = MaxLinkTXRate;
         }

#ifdef DEBUG_PRINT
          printf("link %u, probation starts to %lu\n", LinkIdx,
                 Link[LinkIdx].TxDesiredRate);
#endif
        }
      }
    }
  }
}

void MultiLink::sendRate(uint16_t LinkIdx, uint16_t Rate) {
  uint8_t Buffer4Char[4];
  *((uint16_t *)&Buffer4Char[0]) = LinkIdx;
  *((uint16_t *)&Buffer4Char[2]) = Rate;
  for (int32_t i = 0; i < TotalLinkNumber; i++) {
    if (Link[i].Status == 3) { // send rate over all active links
      sendto(Link[i].SocketFd, Buffer4Char, 4, MSG_DONTWAIT,
             (const struct sockaddr *)&Link[i].Remote, sizeof(sockaddr_in));
      Link[i].TxTotalBytes += 4; // increment tx byte counter
    }
  }
}

// takes input data, computes packet sizes, SendS original Packets and prepares
// data for redundant packet generation
void MultiLink::send(uint16_t Size, uint8_t *DataBuf) {

  // for now, just go over all links and send it, it needs to be replaced with
  // the scheduler
  uint16_t i, SizeWithHeaders, BlockSize, OnePacketSize, NumOrigPackets,
      NumTotalPackets;
  uint32_t Crc;
  int32_t NBytes;

  // printf("sending sequence size %u\n",size);
  if (getActiveLinksNumber() == 0)
    return; // no connection

  SizeWithHeaders = Size + 2 /*payload size*/ + 4 /*crc32*/; // initial length

  // determine original and total packet number
  NumOrigPackets = 0;
  do {
    NumOrigPackets += CrsRateNumerator;
    BlockSize = ceil(1.0 * SizeWithHeaders / NumOrigPackets);
#define ALIGN_CEIL(size, align) ((size + align - 1) & (~(align - 1)))
    BlockSize = ALIGN_CEIL(BlockSize, 4); // it was written that CRS works
                                            // faster if the block size is
                                            // alliged to 4 bytes
    OnePacketSize = BlockSize + 5;       // because of the headers, 5 bytes
  } while (OnePacketSize > PacketSizeMax);

  NumTotalPackets =
      NumOrigPackets / CrsRateNumerator * CrsRateDenominator;

  // compute CRC32
  Crc = crc32_ieee(0, DataBuf, Size);

  // copy data to sequence buffer and add headers + CRC32
  pthread_mutex_lock(&SendMtx); // to make it thread safe
  *((uint16_t *)&SendSequenceDataBuffer[0]) = Size;
  *((uint32_t *)&SendSequenceDataBuffer[2]) = Crc;
  memcpy(&SendSequenceDataBuffer[6], DataBuf, Size);
  if (Size < BlockSize)
    memset(&SendSequenceDataBuffer[6 + Size], 0, BlockSize - Size);
  // form original Packets
  for (i = 0; i < NumOrigPackets; i++) {
    *((uint16_t *)&SendPacketDataBuffer[0]) =
        SendSequenceId; // 2 bytes for sequence ID
    *((uint8_t *)&SendPacketDataBuffer[2]) =
        (uint8_t)i; // 1 byte for packet idx
    *((uint8_t *)&SendPacketDataBuffer[3]) =
        (uint8_t)NumOrigPackets; // 1 byte for original packet number
    *((uint8_t *)&SendPacketDataBuffer[4]) =
        (uint8_t)NumTotalPackets; // 1 byte for total packet number
    memcpy(&SendPacketDataBuffer[5],
           &SendSequenceDataBuffer[i * BlockSize], BlockSize);
    pthread_mutex_lock(&SendPacketWriterMtx);
    WRITE_CHECK(SendPacketPipe[1], &OnePacketSize, 2);
    WRITE_CHECK(SendPacketPipe[1], SendPacketDataBuffer, OnePacketSize);
    pthread_mutex_unlock(&SendPacketWriterMtx);
  }

  // prepare unputs for redundant Packets
  WRITE_CHECK(SendSequencePipe[1], &SendSequenceId, 2);
  WRITE_CHECK(SendSequencePipe[1], &NumOrigPackets, 2);
  WRITE_CHECK(SendSequencePipe[1], &NumTotalPackets, 2);
  WRITE_CHECK(SendSequencePipe[1], &BlockSize, 2);
  WRITE_CHECK(SendSequencePipe[1], SendSequenceDataBuffer,
        BlockSize * NumOrigPackets);
#ifdef DEBUG_PRINT
  printf("sequence of length %u was split into %u Packets of length %u and "
         "encoded into %u Packets\n", Size, NumOrigPackets, OnePacketSize,
         NumTotalPackets);
#endif
  SendSequenceId++;
  SendSequenceId %= MaxSendSequenceId;
  pthread_mutex_unlock(&SendMtx);
}

void MultiLink::runMakeRedundantPackets() {
  uint16_t i, SendSequenceId, BlockSize, NumOrigPackets, NumTotalPackets,
      OnePacketSize;

  while (1) {
    READ_CHECK(SendSequencePipe[0], &SendSequenceId, 2);
    if (SendSequenceId >= MaxSendSequenceId) {
      printf("Error: sen sequence id is greater than maximum allowed\n");
      exit(-1);
    }
    READ_CHECK(SendSequencePipe[0], &NumOrigPackets, 2);
    if (NumOrigPackets >= 256) {
      printf("Error: number of original packets is greater than 256\n");
      exit(-1);
    }
    READ_CHECK(SendSequencePipe[0], &NumTotalPackets, 2);
    if (NumTotalPackets >= 256) {
      printf("Error: number of total packets is greater than 256\n");
      exit(-1);
    }
    READ_CHECK(SendSequencePipe[0], &BlockSize, 2);
    if (BlockSize >= PacketSizeMax) {
      printf("Error: block size is greater than PacketMaxSize\n");
      exit(-1);
    }
    READ_CHECK(SendSequencePipe[0], SendRedundantSequenceDataBuffer,
         BlockSize * NumOrigPackets);
    // form original Packets to encode
    for (i = 0; i < NumTotalPackets; i++)
      EncoderBlocks[i] = &SendRedundantSequenceDataBuffer[i * BlockSize];

    gf_gen_cauchy1_matrix(EncodeMatrix, NumTotalPackets, NumOrigPackets);

    ec_init_tables(NumOrigPackets, NumTotalPackets - NumOrigPackets,
                   &EncodeMatrix[NumOrigPackets * NumOrigPackets], GTbls);

    ec_encode_data(BlockSize, NumOrigPackets, NumTotalPackets - NumOrigPackets,
                   GTbls, EncoderBlocks, &EncoderBlocks[NumOrigPackets]);

    OnePacketSize = BlockSize + 5;
    for (i = NumOrigPackets; i < NumTotalPackets; i++) {
      *((uint16_t *)&SendRedundantPacketDataBuffer[0]) =
          SendSequenceId; // 2 bytes for sequence ID
      *((uint8_t *)&SendRedundantPacketDataBuffer[2]) =
          (uint8_t)i; // 1 byte for packet idx
      *((uint8_t *)&SendRedundantPacketDataBuffer[3]) =
          (uint8_t)NumOrigPackets; // 1 byte for original packet number
      *((uint8_t *)&SendRedundantPacketDataBuffer[4]) =
          (uint8_t)NumTotalPackets; // 1 byte for total packet number
      memcpy(&SendRedundantPacketDataBuffer[5],
             &SendRedundantSequenceDataBuffer[i * BlockSize], BlockSize);
      pthread_mutex_lock(&SendPacketWriterMtx);
      WRITE_CHECK(SendPacketPipe[1], &OnePacketSize, 2);
      WRITE_CHECK(SendPacketPipe[1], SendRedundantPacketDataBuffer,
            OnePacketSize);
      pthread_mutex_unlock(&SendPacketWriterMtx);
    }
  }
}

void MultiLink::runScheduleAndSendPacket() {
  uint16_t i, LinkIdx, PktSize;
  uint64_t CurrentTime;
  int64_t TimeToWait, MinTimeToWait;

  while (1) {
    READ_CHECK(SendPacketPipe[0], &PktSize, 2);
    if (PktSize > PacketSizeMax) {
      printf("Packet size is greater than maximum allowed\n");
      exit(-1);
    }
    READ_CHECK(SendPacketPipe[0], SendPacketSchedulerBuffer, PktSize);
    CurrentTime = getLocalTime();

    // find a link with the minimum estimated waiting time. Waiting time is a
    // function of the link rate and data that was sent
    LinkIdx = 0xFFFF;
    MinTimeToWait = INT64_MAX;
    for (i = 0; i < TotalLinkNumber; i++) {
      if (Link[i].Status == 3) {
        TimeToWait = (int64_t)Link[i].EstimatedFreeTime - (int64_t)CurrentTime;
        if (MinTimeToWait > TimeToWait) {
          LinkIdx = i;
          MinTimeToWait = TimeToWait;
        }
      }
    }
    if (LinkIdx == 0xFFFF)
      return; // unknown errors, no links?
    sendto(Link[LinkIdx].SocketFd, SendPacketSchedulerBuffer, PktSize,
           MSG_DONTWAIT, (const struct sockaddr *)&Link[LinkIdx].Remote,
           sizeof(sockaddr_in));
    Link[LinkIdx].TxTotalBytes += PktSize; // increment tx byte couter
    // increment estimated free time
    if (Link[LinkIdx].EstimatedFreeTime < CurrentTime)
      Link[LinkIdx].EstimatedFreeTime =
          CurrentTime + 1000000 * PktSize / Link[LinkIdx].TxDesiredRate;
    else
      Link[LinkIdx].EstimatedFreeTime +=
          1000000 * PktSize / Link[LinkIdx].TxDesiredRate;
  }
}

void MultiLink::runDecoder() {
  uint16_t i, j, r, p, k, PktSize, SequenceId, OrigPacketNum, TotalPacketNum,
      PacketIdx, SeqSize;
  uint32_t CrcAsIs, Crc;
  bool TestFlag;
  int32_t NBytes;

  CorrectSequences = 0;
  IncorrectSequences = 0;
  CorrectSequencesDropped = 0;
  // reset table
  for (i = 0; i < MaxSendSequenceId; i++) {
    DecoderSequences[i].Forbidden = false;
    DecoderSequences[i].AvailableBlockNumber = 0;
    DecoderSequences[i].Packets = NULL;
  }

  while (1) {
    READ_CHECK(ReceivePacketPipe[0], &PktSize, 2);
    if (PktSize > PacketSizeMax) {
      printf("Packet size is greater than maximum allowed\n");
      exit(-1);
    }
    READ_CHECK(ReceivePacketPipe[0], ReceivePacketDataBuffer, PktSize);
#ifdef DEBUG_PRINT
    printf("decoding packet of size %u, header %u\n", PktSize,
           *((uint16_t *)ReceivePacketDataBuffer));
#endif
    // parse packet header
    SequenceId = *(
        (uint16_t *)&ReceivePacketDataBuffer[0]); // 2 bytes for sequence ID
    PacketIdx =
        *((uint8_t *)&ReceivePacketDataBuffer[2]); // 1 byte for packet idx
    OrigPacketNum =
        *((uint8_t *)&ReceivePacketDataBuffer[3]); // 1 byte for original
                                                      // packet number
    TotalPacketNum = *((
        uint8_t
            *)&ReceivePacketDataBuffer[4]); // 1 byte for total packet number

    // check if the sequence id is valid
    if (SequenceId >= MaxSendSequenceId)
      continue; // error, frop the packet
    // check if sequence id is forbidden
    if (DecoderSequences[SequenceId].Forbidden)
      continue; // too early, drop the packet

    // store in table
    if (DecoderSequences[SequenceId].AvailableBlockNumber == 0) {
      // first packet, new sequence. allocate memory and set params
      DecoderSequences[SequenceId].OriginalBlockNumber = OrigPacketNum;
      DecoderSequences[SequenceId].TotalBlockNumber = TotalPacketNum;
      DecoderSequences[SequenceId].PacketSize = PktSize;
      DecoderSequences[SequenceId].Packets =
          (uint8_t *)malloc(((uint32_t)PktSize) * OrigPacketNum);
      ALLOC_CHECK(DecoderSequences[SequenceId].Packets,
                  "Decoder sequence packets");
    } else {
      // check if params are the same as we have in the table
      if (DecoderSequences[SequenceId].OriginalBlockNumber !=
          OrigPacketNum)
        continue; // mismatch
      if (DecoderSequences[SequenceId].TotalBlockNumber != TotalPacketNum)
        continue; // mismatch
      if (DecoderSequences[SequenceId].PacketSize != PktSize)
        continue; // mismatch
      // check index
      TestFlag = false;
      for (i = 0; i < DecoderSequences[SequenceId].AvailableBlockNumber;
           i++)
        if (*((uint8_t *)&DecoderSequences[SequenceId]
                  .Packets[i * DecoderSequences[SequenceId].PacketSize +
                           2]) == PacketIdx)
          TestFlag = true;
      if (TestFlag)
        continue; // alrady have the same index. This is unexpected, but
                  // overprotection is OK
    }
    memcpy(&DecoderSequences[SequenceId]
                .Packets[DecoderSequences[SequenceId].AvailableBlockNumber *
                         DecoderSequences[SequenceId].PacketSize],
           ReceivePacketDataBuffer, PktSize);
    DecoderSequences[SequenceId].AvailableBlockNumber++;

    // check if we have enough Packets to decode
    if (DecoderSequences[SequenceId].AvailableBlockNumber ==
        OrigPacketNum) {

    int32_t BlockBytes = PktSize - 5;
    int32_t NErrs = 0;
    for (i = 0; i < OrigPacketNum; i++) {
      DecodeIndex[i] =
          *((uint8_t *)&DecoderSequences[SequenceId]
                .Packets[i * DecoderSequences[SequenceId].PacketSize + 2]);
      RecoverSrcs[i] = &DecoderSequences[SequenceId]
          .Packets[i * DecoderSequences[SequenceId].PacketSize + 5];
    }
    uint8_t DecodeFlag[256];
    for (i = 0; i < TotalPacketNum; i++)
      DecodeFlag[i] = 0;
    for (i = 0; i < OrigPacketNum; i++) {
      DecodeFlag[DecodeIndex[i]] = 1;
    }
    for (i = 0; i < TotalPacketNum; i++)
      if (DecodeFlag[i] == 0)
        ErrList[NErrs++] = i;
    if (NErrs != TotalPacketNum - OrigPacketNum) {
      printf("Fatal error %d != %d - %d!\n", NErrs, TotalPacketNum, OrigPacketNum);
      exit(-1);
    }

    gf_gen_cauchy1_matrix(EncodeMatrix2, TotalPacketNum, OrigPacketNum);

    for (i = 0, r = 0; i < OrigPacketNum; i++, r++)
      for (j = 0; j < OrigPacketNum; j++)
        TempMatrix[OrigPacketNum * i + j] =
            EncodeMatrix2[OrigPacketNum * DecodeIndex[i] + j];

    if (gf_invert_matrix(TempMatrix, InvertMatrix, OrigPacketNum) < 0) {
      printf("Fail on generate decode matrix\n");
      exit(-1);
    }

    for (i = 0; i < NErrs; i++)
      if (ErrList[i] < OrigPacketNum) // A src err
        for (j = 0; j < OrigPacketNum; j++)
          DecodeMatrix[OrigPacketNum * i + j] =
            InvertMatrix[OrigPacketNum * ErrList[i] + j];

    for (p = 0; p < NErrs; p++) {
      if (ErrList[p] >= OrigPacketNum) {// A parity err
        for (i = 0; i < OrigPacketNum; i++) {
          uint8_t s = 0;
          for (j = 0; j < OrigPacketNum; j++)
            s ^= gf_mul(InvertMatrix[j * OrigPacketNum + i],
                        EncodeMatrix2[OrigPacketNum * ErrList[p] + j]);
          DecodeMatrix[OrigPacketNum * p + i] = s;
        }
      }
    }

    // Recover data
    ec_init_tables(OrigPacketNum, NErrs, DecodeMatrix, GTblsd);
    ec_encode_data(BlockBytes, OrigPacketNum, NErrs, GTblsd,
                   RecoverSrcs, RecoverOutp);

    for (i = 0; i < OrigPacketNum; i++) {
      memcpy(&DecodedSequenceBuffer[DecodeIndex[i] * BlockBytes],
             RecoverSrcs[i], BlockBytes);
    }
    for (i = 0; i < NErrs; i++) {
      memcpy(&DecodedSequenceBuffer[ErrList[i] * BlockBytes],
             RecoverOutp[i], BlockBytes);
    }

    // check crc
    SeqSize = *((uint16_t *)&DecodedSequenceBuffer[0]);
    CrcAsIs = *((uint32_t *)&DecodedSequenceBuffer[2]);
    Crc = crc32_ieee(0, &DecodedSequenceBuffer[6], SeqSize);
    if (Crc == CrcAsIs) {
      CorrectSequences++;
        // correct reconstruction: write sequence to the output buffer
        ioctl(ReceiveSequencePipe[0], FIONREAD, &NBytes);
        if ((ReceiveSequencePipeSize >> 1) > NBytes) {
          // check for enough space in the pipe. A workaround: stop
          // if 1/2 of the pipe size is reached (becasure it is not
          // possible to know the real free space as pipes store
          // recorts in pages of 4096 blocks)
          WRITE_CHECK(ReceiveSequencePipe[1], &SeqSize, 2);
          WRITE_CHECK(ReceiveSequencePipe[1], &DecodedSequenceBuffer[6], SeqSize);
        } else
          CorrectSequencesDropped++; // no free space, drop the sequence
      } else
        IncorrectSequences++; // CRC does not match, drop the sequence
      // clear all sequence Packets from buffer
      DecoderSequences[SequenceId].AvailableBlockNumber = 0;
      free(DecoderSequences[SequenceId].Packets);
      DecoderSequences[SequenceId].Forbidden = true;

      // clear forbidden flags and reset data all sequences that have ids later
      // than this one by 1/2 of the buffer
      for (i = MaxSendSequenceId / 4; i < 3 * MaxSendSequenceId / 4; i++) {
        k = (SequenceId + i);
        if (k >= MaxSendSequenceId)
          k -= MaxSendSequenceId;
//        k = (SequenceId + i) % MaxSendSequenceId;
        DecoderSequences[k].Forbidden = false;
        if (DecoderSequences[k].AvailableBlockNumber > 0) {
          DecoderSequences[k].AvailableBlockNumber = 0;
          free(DecoderSequences[k].Packets);
        }
      }
    }
  }
}

void MultiLink::receive(uint16_t *Size, uint8_t *DataBuf) {
  pthread_mutex_lock(&ReceiveMtx);
  READ_CHECK(ReceiveSequencePipe[0], Size, 2);
  READ_CHECK(ReceiveSequencePipe[0], DataBuf, *Size);
  pthread_mutex_unlock(&ReceiveMtx);
}

uint16_t MultiLink::findRemoteIndexByAddr(uint16_t LocalSocketIdx,
                                          struct sockaddr_in SenderAddr) {
  uint16_t i;
  // check if we have this sender address in the links
  for (i = 0; i < RemoteCommDeviceNumber; i++) {
    if ((Link[LocalSocketIdx * RemoteCommDeviceNumber + i]
             .Remote.sin_addr.s_addr == SenderAddr.sin_addr.s_addr) &&
        (Link[LocalSocketIdx * RemoteCommDeviceNumber + i]
             .Remote.sin_port == SenderAddr.sin_port)) {
      break;
    }
  }
  return i;
};

uint64_t MultiLink::getRate() {
  uint64_t i, Result;
  Result = 0;
  for (i = 0; i < TotalLinkNumber; i++)
    if (Link[i].Status == 3)
      Result += Link[i].TxDesiredRate;
  return (uint64_t)(1.0 * Result / CrsRateDenominator * CrsRateNumerator);
}

void MultiLink::printLinksInfo() {
  uint16_t Idx;
  for (Idx = 0; Idx < TotalLinkNumber; Idx++) {
    printf("link: %02u, RemoteIdx: %02u, Status: %u, from socket %d to %s:%d, "
           "tx bytes %lu, rx bytes %lu\n",
           Idx, Link[Idx].RemoteLinkIdx, Link[Idx].Status, Link[Idx].SocketFd,
           inet_ntoa(Link[Idx].Remote.sin_addr),
           ntohs(Link[Idx].Remote.sin_port), Link[Idx].TxTotalBytes,
           Link[Idx].RxTotalBytes);
  }
  printf(
      "correct seq total=%lu, dropped correct seq = %lu, incorrect seq = %lu\n",
      CorrectSequences, CorrectSequencesDropped, IncorrectSequences);
}

uint16_t MultiLink::getActiveLinksNumber() {
  uint16_t i, Result;
  Result = 0;
  for (i = 0; i < TotalLinkNumber; i++)
    if (Link[i].Status == 3)
      Result++;
  return Result;
}

void MultiLink::setRedundancy(uint8_t K, uint8_t N) {
  CrsRateNumerator = K;
  CrsRateDenominator = N;
}

uint64_t MultiLink::getLocalTime() {
  // may need to replace it with a better way to get time, but for now...
  struct timeval t0;
  gettimeofday(&t0, NULL);
  return t0.tv_sec * 1000000 + t0.tv_usec;
}
